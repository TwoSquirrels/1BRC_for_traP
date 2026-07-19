package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"runtime/pprof"
	"strconv"
	"strings"
)

const (
	maxWorkers       = 8
	readChunkSize    = 8 * 1024 * 1024
	channelTableSize = 1 << 15
	channelTableMask = channelTableSize - 1
	maxChannels      = 10_000

	fnv64Offset = uint64(14695981039346656037)
	fnv64Prime  = uint64(1099511628211)
	unix2027    = uint32(1798761600)
	secondsDay  = uint32(24 * 60 * 60)
)

type aggregateStats struct {
	minLength uint32
	maxLength uint32
	total     uint32
	count     uint32
	stamps    uint32
}

type channelInfo struct {
	hash    uint64
	channel string
}

type monthlyStats [12]aggregateStats

// workerTable keeps the lookup keys separate from the larger aggregate data.
// hashes and ids stay small enough to remain cache-resident while parsing.
type workerTable struct {
	hashes   [channelTableSize]uint64
	ids      [channelTableSize]uint16 // channel ID + 1; zero means unused
	channels []channelInfo
	stats    []monthlyStats
}

type chunkJob struct {
	data   []byte
	buffer []byte // non-nil when this job owns a pooled read buffer
}

var monthByDay = func() [365]uint8 {
	var result [365]uint8
	days := [...]int{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
	day := 0
	for month, count := range days {
		for range count {
			result[day] = uint8(month)
			day++
		}
	}
	return result
}()

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: program input.csv output.txt")
		os.Exit(2)
	}

	profileDir := os.Getenv("PPROF_DIR")
	cpuProfilePath := ""
	if profileDir != "" {
		cpuProfilePath = filepath.Join(profileDir, "cpu.pprof")
	}
	stopCPU := startCPUProfile(cpuProfilePath)

	in, err := os.Open(os.Args[1])
	if err != nil {
		fatal(err)
	}
	defer in.Close()

	result, err := aggregate(in)
	if err != nil {
		fatal(err)
	}

	out, err := os.Create(os.Args[2])
	if err != nil {
		fatal(err)
	}
	if err := writeResult(out, result); err != nil {
		out.Close()
		fatal(err)
	}
	if err := out.Close(); err != nil {
		fatal(err)
	}

	stopCPU()
	if profileDir != "" {
		writeHeapProfile(filepath.Join(profileDir, "memory.pprof"))
		runtime.KeepAlive(result)
	}
}

func newWorkerTable() *workerTable {
	return &workerTable{
		channels: make([]channelInfo, 0, maxChannels),
		stats:    make([]monthlyStats, 0, maxChannels),
	}
}

func aggregate(in *os.File) (*workerTable, error) {
	workers := runtime.GOMAXPROCS(0)
	if workers > maxWorkers {
		workers = maxWorkers
	}
	if workers < 1 {
		workers = 1
	}

	jobs := make(chan chunkJob, workers*2)
	freeBuffers := make(chan []byte, workers+2)
	results := make(chan *workerTable, workers)
	for range workers + 2 {
		freeBuffers <- make([]byte, readChunkSize)
	}
	for range workers {
		go aggregateWorker(jobs, freeBuffers, results)
	}

	readErr := feedChunks(in, jobs, freeBuffers)
	close(jobs)

	tables := make([]*workerTable, 0, workers)
	for range workers {
		tables = append(tables, <-results)
	}
	if readErr != nil {
		return nil, readErr
	}
	return mergeTables(tables), nil
}

func feedChunks(in *os.File, jobs chan<- chunkJob, freeBuffers chan []byte) error {
	headerPending := true
	var carry []byte

	for {
		buffer := <-freeBuffers
		n, err := in.Read(buffer)
		if n == 0 {
			freeBuffers <- buffer
			if err == io.EOF {
				break
			}
			if err != nil {
				return err
			}
			continue
		}

		data := buffer[:n]
		start := 0

		// Finish a line which crossed the previous read boundary. Its storage is
		// independent of the pooled buffer, so both jobs may run concurrently.
		if len(carry) != 0 {
			newline := bytes.IndexByte(data, '\n')
			if newline < 0 {
				carry = append(carry, data...)
				freeBuffers <- buffer
				if err != nil && err != io.EOF {
					return err
				}
				if err == io.EOF {
					break
				}
				continue
			}
			carry = append(carry, data[:newline+1]...)
			if headerPending {
				headerPending = false
			} else {
				jobs <- chunkJob{data: carry}
			}
			carry = nil
			start = newline + 1
		}

		if headerPending {
			newline := bytes.IndexByte(data[start:], '\n')
			if newline < 0 {
				carry = append(carry, data[start:]...)
				freeBuffers <- buffer
				if err != nil && err != io.EOF {
					return err
				}
				if err == io.EOF {
					break
				}
				continue
			}
			headerPending = false
			start += newline + 1
		}

		if start < n {
			lastNewline := bytes.LastIndexByte(data[start:], '\n')
			if lastNewline < 0 {
				carry = append(carry, data[start:]...)
				freeBuffers <- buffer
			} else {
				end := start + lastNewline + 1
				if end < n {
					carry = append(carry, data[end:]...)
				}
				jobs <- chunkJob{data: data[start:end], buffer: buffer}
			}
		} else {
			freeBuffers <- buffer
		}

		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
	}

	if headerPending {
		return fmt.Errorf("input is missing its header line")
	}
	if len(carry) != 0 {
		return fmt.Errorf("input does not end with LF")
	}
	return nil
}

func aggregateWorker(jobs <-chan chunkJob, freeBuffers chan<- []byte, results chan<- *workerTable) {
	table := newWorkerTable()
	for job := range jobs {
		parseChunk(job.data, table)
		if job.buffer != nil {
			freeBuffers <- job.buffer
		}
	}
	results <- table
}

// parseChunk parses complete LF-terminated rows. The input format is guaranteed
// by the contest, so the hot loop deliberately avoids redundant validation.
func parseChunk(data []byte, table *workerTable) {
	for pos := 0; pos < len(data); {
		timestamp := uint32(data[pos]-'0')*1_000_000_000 +
			uint32(data[pos+1]-'0')*100_000_000 +
			uint32(data[pos+2]-'0')*10_000_000 +
			uint32(data[pos+3]-'0')*1_000_000 +
			uint32(data[pos+4]-'0')*100_000 +
			uint32(data[pos+5]-'0')*10_000 +
			uint32(data[pos+6]-'0')*1_000 +
			uint32(data[pos+7]-'0')*100 +
			uint32(data[pos+8]-'0')*10 +
			uint32(data[pos+9]-'0')
		month := int(monthByDay[(timestamp-unix2027)/secondsDay])
		pos += 11 // ten timestamp digits and comma

		channelStart := pos
		channelHash := fnv64Offset
		for data[pos] != ',' {
			channelHash ^= uint64(data[pos])
			channelHash *= fnv64Prime
			pos++
		}
		channel := data[channelStart:pos]
		pos++

		var length uint32
		for data[pos] != ',' {
			length = length*10 + uint32(data[pos]-'0')
			pos++
		}
		pos++

		var stamps uint32
		for data[pos] != '\n' {
			stamps = stamps*10 + uint32(data[pos]-'0')
			pos++
		}
		pos++

		slot := int((channelHash ^ (channelHash >> 32)) & channelTableMask)
		var channelID int
		for {
			id := table.ids[slot]
			if id == 0 {
				channelID = len(table.channels)
				table.hashes[slot] = channelHash
				table.ids[slot] = uint16(channelID + 1)
				table.channels = append(table.channels, channelInfo{
					hash:    channelHash,
					channel: string(channel),
				})
				table.stats = append(table.stats, monthlyStats{})
				break
			}
			if table.hashes[slot] == channelHash {
				channelID = int(id) - 1
				break
			}
			slot = (slot + 1) & channelTableMask
		}

		stats := &table.stats[channelID][month]
		if stats.count == 0 {
			stats.minLength = length
			stats.maxLength = length
		} else {
			if length < stats.minLength {
				stats.minLength = length
			}
			if length > stats.maxLength {
				stats.maxLength = length
			}
		}
		stats.total += length
		stats.count++
		stats.stamps += stamps
	}
}

func mergeTables(tables []*workerTable) *workerTable {
	result := newWorkerTable()
	for _, table := range tables {
		for i := range table.channels {
			entry := &table.channels[i]
			slot := int((entry.hash ^ (entry.hash >> 32)) & channelTableMask)
			var channelID int
			for {
				id := result.ids[slot]
				if id == 0 {
					channelID = len(result.channels)
					result.hashes[slot] = entry.hash
					result.ids[slot] = uint16(channelID + 1)
					result.channels = append(result.channels, channelInfo{
						hash:    entry.hash,
						channel: entry.channel,
					})
					result.stats = append(result.stats, monthlyStats{})
					break
				}
				if result.hashes[slot] == entry.hash {
					channelID = int(id) - 1
					break
				}
				slot = (slot + 1) & channelTableMask
			}

			for month := range 12 {
				source := &table.stats[i][month]
				if source.count == 0 {
					continue
				}
				destination := &result.stats[channelID][month]
				if destination.count == 0 {
					*destination = *source
					continue
				}
				if source.minLength < destination.minLength {
					destination.minLength = source.minLength
				}
				if source.maxLength > destination.maxLength {
					destination.maxLength = source.maxLength
				}
				destination.total += source.total
				destination.count += source.count
				destination.stamps += source.stamps
			}
		}
	}
	return result
}

func writeResult(out *os.File, result *workerTable) error {
	w := bufio.NewWriterSize(out, 1024*1024)
	b := &strings.Builder{}
	for i := range result.channels {
		entry := &result.channels[i]
		for month := range 12 {
			stats := &result.stats[i][month]
			if stats.count == 0 {
				continue
			}

			b.Reset()
			b.WriteString(entry.channel)
			b.WriteString(",2027-")
			b.WriteByte(byte((month+1)/10) + '0')
			b.WriteByte(byte((month+1)%10) + '0')
			b.WriteByte('=')
			b.WriteString(strconv.Itoa(int(stats.minLength)))
			b.WriteByte('/')
			average := float64(stats.total) / float64(stats.count)
			b.WriteString(strconv.FormatFloat(average, 'f', 2, 64))
			b.WriteByte('/')
			b.WriteString(strconv.Itoa(int(stats.maxLength)))
			b.WriteByte('/')
			b.WriteString(strconv.Itoa(int(stats.count)))
			b.WriteByte('/')
			b.WriteString(strconv.Itoa(int(stats.stamps)))
			b.WriteByte('\n')
			if _, err := w.WriteString(b.String()); err != nil {
				return err
			}
		}
	}
	return w.Flush()
}

func startCPUProfile(path string) func() {
	if path == "" {
		return func() {}
	}
	f, err := os.Create(path)
	if err != nil {
		fatal(fmt.Errorf("create CPU profile: %w", err))
	}
	if err := pprof.StartCPUProfile(f); err != nil {
		f.Close()
		fatal(fmt.Errorf("start CPU profile: %w", err))
	}
	return func() {
		pprof.StopCPUProfile()
		if err := f.Close(); err != nil {
			fatal(fmt.Errorf("close CPU profile: %w", err))
		}
	}
}

func writeHeapProfile(path string) {
	f, err := os.Create(path)
	if err != nil {
		fatal(fmt.Errorf("create heap profile: %w", err))
	}
	runtime.GC()
	if err := pprof.WriteHeapProfile(f); err != nil {
		f.Close()
		fatal(fmt.Errorf("write heap profile: %w", err))
	}
	if err := f.Close(); err != nil {
		fatal(fmt.Errorf("close heap profile: %w", err))
	}
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}
