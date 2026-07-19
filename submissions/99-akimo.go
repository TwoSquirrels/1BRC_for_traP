package main

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"math/bits" // SWARビットマジック用のイントリンシック
	"os"
	"runtime/pprof"
	"strconv"
	"sync"
	"syscall"
	"unsafe"
)

type Stats struct {
	MinLength       uint32
	MaxLength       uint32
	TotalLength     uint32
	MessageCount    uint32
	TotalStampCount uint32
}

type Entry struct {
	Hash            uint64
	KeyMonthCode    uint32
	MinLength       uint32
	MaxLength       uint32
	TotalLength     uint32
	MessageCount    uint32
	TotalStampCount uint32
}

type ChanRef struct {
	Ptr uintptr
	Len uint32
}

type FinalEntry struct {
	Hash            uint64
	MonthCode       uint32
	ChanPtr         uintptr
	ChanLen         uint32
	MinLength       uint32
	MaxLength       uint32
	TotalLength     uint32
	MessageCount    uint32
	TotalStampCount uint32
}

var monthStrs = [...]string{
	"", "2027-01", "2027-02", "2027-03", "2027-04", "2027-05", "2027-06",
	"2027-07", "2027-08", "2027-09", "2027-10", "2027-11", "2027-12",
}

var yearMonthTable [365]uint32

func initYearMonthTable() {
	daysInMonth := [...]int{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
	dayIdx := 0
	for m := 1; m <= 12; m++ {
		days := daysInMonth[m-1]
		for range days {
			yearMonthTable[dayIdx] = uint32(202700 + m)
			dayIdx++
		}
	}
}

func main() {
	usePprof := flag.Bool("pprof", false, "Enable pprof profiling")
	flag.Parse()

	if *usePprof {
		f, err := os.Create("cpu.prof")
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error creating pprof file: %v\n", err)
			os.Exit(1)
		}
		defer f.Close()

		if err := pprof.StartCPUProfile(f); err != nil {
			fmt.Fprintf(os.Stderr, "Error starting CPU profile: %v\n", err)
			os.Exit(1)
		}
		defer pprof.StopCPUProfile()
	}

	initYearMonthTable()

	args := flag.Args()
	if len(args) < 2 {
		fmt.Fprintln(os.Stderr, "Usage: ./program <input_csv> <output_txt>")
		os.Exit(1)
	}
	inputPath := args[0]
	outputPath := args[1]

	inputFile, err := os.Open(inputPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error opening input file: %v\n", err)
		os.Exit(1)
	}
	defer inputFile.Close()

	outputFile, err := os.Create(outputPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error creating output file: %v\n", err)
		os.Exit(1)
	}
	defer outputFile.Close()

	fi, err := inputFile.Stat()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error statting input file: %v\n", err)
		os.Exit(1)
	}
	size := fi.Size()

	if size == 0 {
		fmt.Fprintf(os.Stderr, "Error reading header: EOF\n")
		os.Exit(1)
	}

	data, err := syscall.Mmap(int(inputFile.Fd()), 0, int(size), syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error mmapping file: %v\n", err)
		os.Exit(1)
	}
	defer syscall.Munmap(data)

	syscall.Syscall(syscall.SYS_MADVISE, uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)), syscall.MADV_WILLNEED)

	headerEnd := bytes.IndexByte(data, '\n')
	if headerEnd == -1 {
		fmt.Fprintf(os.Stderr, "Error reading header: no newline found\n")
		os.Exit(1)
	}
	startOffset := int64(headerEnd + 1)

	numWorkers := 8
	type Chunk struct {
		start int64
		end   int64
	}
	chunks := make([]Chunk, numWorkers)

	totalDataSize := size - startOffset
	step := totalDataSize / int64(numWorkers)

	current := startOffset
	for i := range numWorkers {
		chunks[i].start = current
		if i == numWorkers-1 {
			chunks[i].end = size
			break
		}

		guessEnd := current + step
		if guessEnd >= size {
			chunks[i].end = size
			current = size
			continue
		}

		remain := data[guessEnd:]
		nextNL := bytes.IndexByte(remain, '\n')
		if nextNL == -1 {
			chunks[i].end = size
			current = size
		} else {
			chunks[i].end = guessEnd + int64(nextNL) + 1
			current = chunks[i].end
		}
	}

	workerResults := make([][]Entry, numWorkers)
	workerChans := make([][]ChanRef, numWorkers)
	var wg sync.WaitGroup

	for i := range numWorkers {
		if chunks[i].start >= chunks[i].end {
			continue
		}
		wg.Add(1)
		go func(workerID int, start, end int64) {
			defer wg.Done()

			workerData := data[start:end]
			if len(workerData) == 0 {
				return
			}

			tableSize := 262144 // 2^18
			mask := tableSize - 1
			table := make([]Entry, tableSize)
			chanRefs := make([]ChanRef, tableSize)

			for len(workerData) > 0 {
				nextNL := bytes.IndexByte(workerData, '\n')
				var line []byte
				if nextNL == -1 {
					line = workerData
					workerData = nil
				} else {
					line = workerData[:nextNL]
					workerData = workerData[nextNL+1:]
				}

				if len(line) > 0 {
					pBase := uintptr(unsafe.Pointer(&line[0]))

					unixTime := int64(*(*byte)(unsafe.Pointer(pBase))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 1))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 2))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 3))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 4))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 5))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 6))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 7))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 8))) - '0'
					unixTime = unixTime*10 + int64(*(*byte)(unsafe.Pointer(pBase + 9))) - '0'

					pTail := len(line) - 1

					// ====================================================
					// ⚡️ 超速SWAR：スタンプ数のビット反転算術パース（1〜3桁ブランクレス）
					// ====================================================
					v32Stamp := *(*uint32)(unsafe.Pointer(pBase + uintptr(pTail-3)))
					vRevStamp := bits.ReverseBytes32(v32Stamp)

					x32Stamp := vRevStamp ^ 0x2C2C2C2C
					matchesStamp := (x32Stamp - 0x01010101) & ^x32Stamp & 0x80808080

					var stampCount uint32
					if matchesStamp != 0 {
						// 通常パス：1〜3桁（99.9%の行）。一の位・十の位・百の位の位置が固定整列
						trailingZerosStamp := bits.TrailingZeros32(matchesStamp)
						stampLen := trailingZerosStamp >> 3 // 実際の数字の桁数 (1, 2, 3)

						maskStamp := uint32(0xFFFFFFFF) >> ((4 - stampLen) * 8)
						digitsStamp := (vRevStamp & maskStamp) - (0x30303030 & maskStamp)

						// 分岐予測を一切汚さない超軽量な1レイ算術合成
						stampCount = (digitsStamp & 0xFF) + ((digitsStamp >> 8) & 0xFF)*10 + (digitsStamp >> 16)*100
						pTail -= int(stampLen + 1)
					} else {
						// 予測超高確率なコールドパス：4桁以上のフォールバック（ループ）
						stampCount = 0
						multiplier := uint32(1)
						for {
							b := *(*byte)(unsafe.Pointer(pBase + uintptr(pTail)))
							if b == ',' {
								pTail--
								break
							}
							stampCount += uint32(b-'0') * multiplier
							multiplier *= 10
							pTail--
						}
					}
					// ====================================================

					// ====================================================
					// ⚡️ 超速SWAR：メッセージ長のビット反転算術パース（1〜3桁ブランクレス）
					// ====================================================
					v32Msg := *(*uint32)(unsafe.Pointer(pBase + uintptr(pTail-3)))
					vRevMsg := bits.ReverseBytes32(v32Msg)

					x32Msg := vRevMsg ^ 0x2C2C2C2C
					matchesMsg := (x32Msg - 0x01010101) & ^x32Msg & 0x80808080

					var msgLength uint32
					if matchesMsg != 0 {
						// 通常パス：1〜3桁。桁数変動があっても完全ブランクレスで超高速処理
						trailingZerosMsg := bits.TrailingZeros32(matchesMsg)
						msgLen := trailingZerosMsg >> 3 // 実際の数字の桁数 (1, 2, 3)

						maskMsg := uint32(0xFFFFFFFF) >> ((4 - msgLen) * 8)
						digitsMsg := (vRevMsg & maskMsg) - (0x30303030 & maskMsg)

						// 分岐予測を一切汚さない超軽量な1レイ算術合成
						msgLength = (digitsMsg & 0xFF) + ((digitsMsg >> 8) & 0xFF)*10 + (digitsMsg >> 16)*100
						pTail -= int(msgLen)
					} else {
						// 予測超高確率なコールドパス：4桁以上のフォールバック（ループ）
						msgLength = 0
						multiplierMsg := uint32(1)
						for {
							b := *(*byte)(unsafe.Pointer(pBase + uintptr(pTail)))
							if b == ',' {
								break
							}
							msgLength += uint32(b-'0') * multiplierMsg
							multiplierMsg *= 10
							pTail--
						}
					}
					// ====================================================

					chanStart := 11
					chanLen := pTail - chanStart

					d := int((unixTime - 1798761600) / 86400)
					var monthCode uint32
					if d >= 0 && d < 365 {
						monthCode = yearMonthTable[d]
					} else {
						monthCode = 202701
					}

					// ====================================================
					// [真の最適化] コンパイラ駆動型ジャンプテーブル FxHash
					// ====================================================
					var hash uint64 = 0
					const k uint64 = 0x517cc1b727220a95
					pChan := pBase + uintptr(chanStart)

					if chanLen <= 32 {
						switch chanLen {
						case 1:
							v := uint64(*(*byte)(unsafe.Pointer(pChan)))
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 2:
							v := uint64(*(*uint16)(unsafe.Pointer(pChan)))
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 3:
							v := uint64(*(*uint16)(unsafe.Pointer(pChan))) | (uint64(*(*byte)(unsafe.Pointer(pChan + 2))) << 16)
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 4:
							v := uint64(*(*uint32)(unsafe.Pointer(pChan)))
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 5:
							v := uint64(*(*uint32)(unsafe.Pointer(pChan))) | (uint64(*(*byte)(unsafe.Pointer(pChan + 4))) << 32)
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 6:
							v := uint64(*(*uint32)(unsafe.Pointer(pChan))) | (uint64(*(*uint16)(unsafe.Pointer(pChan + 4))) << 32)
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 7:
							v := uint64(*(*uint32)(unsafe.Pointer(pChan))) | (uint64(*(*uint16)(unsafe.Pointer(pChan + 4))) << 32) | (uint64(*(*byte)(unsafe.Pointer(pChan + 6))) << 48)
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 8:
							v := *(*uint64)(unsafe.Pointer(pChan))
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						case 9, 10, 11, 12, 13, 14, 15:
							v0 := *(*uint64)(unsafe.Pointer(pChan))
							hash = (((hash << 5) | (hash >> 59)) ^ v0) * k
							remShift := (8 - (chanLen & 7)) * 8
							v1 := (*(*uint64)(unsafe.Pointer(pChan + uintptr(chanLen-8)))) >> remShift
							hash = (((hash << 5) | (hash >> 59)) ^ v1) * k
						case 16:
							v0 := *(*uint64)(unsafe.Pointer(pChan))
							hash = (((hash << 5) | (hash >> 59)) ^ v0) * k
							v1 := *(*uint64)(unsafe.Pointer(pChan + 8))
							hash = (((hash << 5) | (hash >> 59)) ^ v1) * k
						case 17, 18, 19, 20, 21, 22, 23:
							v0 := *(*uint64)(unsafe.Pointer(pChan))
							hash = (((hash << 5) | (hash >> 59)) ^ v0) * k
							v1 := *(*uint64)(unsafe.Pointer(pChan + 8))
							hash = (((hash << 5) | (hash >> 59)) ^ v1) * k
							remShift := (8 - (chanLen & 7)) * 8
							v2 := (*(*uint64)(unsafe.Pointer(pChan + uintptr(chanLen-8)))) >> remShift
							hash = (((hash << 5) | (hash >> 59)) ^ v2) * k
						case 24:
							v0 := *(*uint64)(unsafe.Pointer(pChan))
							hash = (((hash << 5) | (hash >> 59)) ^ v0) * k
							v1 := *(*uint64)(unsafe.Pointer(pChan + 8))
							hash = (((hash << 5) | (hash >> 59)) ^ v1) * k
							v2 := *(*uint64)(unsafe.Pointer(pChan + 16))
							hash = (((hash << 5) | (hash >> 59)) ^ v2) * k
						default: // 25 ~ 32
							v0 := *(*uint64)(unsafe.Pointer(pChan))
							hash = (((hash << 5) | (hash >> 59)) ^ v0) * k
							v1 := *(*uint64)(unsafe.Pointer(pChan + 8))
							hash = (((hash << 5) | (hash >> 59)) ^ v1) * k
							v2 := *(*uint64)(unsafe.Pointer(pChan + 16))
							hash = (((hash << 5) | (hash >> 59)) ^ v2) * k
							remShift := (8 - (chanLen & 7)) * 8
							v3 := (*(*uint64)(unsafe.Pointer(pChan + uintptr(chanLen-8)))) >> remShift
							hash = (((hash << 5) | (hash >> 59)) ^ v3) * k
						}
					} else {
						idx := 0
						for idx+8 <= chanLen {
							v := *(*uint64)(unsafe.Pointer(pChan + uintptr(idx)))
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
							idx += 8
						}
						if idx < chanLen {
							remShift := (8 - (chanLen & 7)) * 8
							v := (*(*uint64)(unsafe.Pointer(pChan + uintptr(chanLen-8)))) >> remShift
							hash = (((hash << 5) | (hash >> 59)) ^ v) * k
						}
					}

					hash = ((hash << 5) | (hash >> 59)) ^ uint64(monthCode)
					hash *= k
					// ====================================================

					h := hash & uint64(mask)
					for {
						entry := &table[h]
						if entry.MessageCount == 0 {
							entry.Hash = hash
							entry.KeyMonthCode = monthCode

							chanRefs[h] = ChanRef{
								Ptr: pBase + uintptr(chanStart),
								Len: uint32(chanLen),
							}

							entry.MinLength = msgLength
							entry.MaxLength = msgLength
							entry.TotalLength = msgLength
							entry.MessageCount = 1
							entry.TotalStampCount = stampCount
							break
						}
						if entry.Hash == hash && entry.KeyMonthCode == monthCode {
							if msgLength < entry.MinLength {
								entry.MinLength = msgLength
							}
							if msgLength > entry.MaxLength {
								entry.MaxLength = msgLength
							}
							entry.TotalLength += msgLength
							entry.MessageCount++
							entry.TotalStampCount += stampCount
							break
						}
						h = (h + 1) & uint64(mask)
					}
				}
			}
			workerResults[workerID] = table
			workerChans[workerID] = chanRefs
		}(i, chunks[i].start, chunks[i].end)
	}

	wg.Wait()

	finalTableSize := 524288
	finalMask := finalTableSize - 1
	finalTable := make([]FinalEntry, finalTableSize)

	for workerID, table := range workerResults {
		if table == nil {
			continue
		}
		refs := workerChans[workerID]
		for i := range table {
			if table[i].MessageCount == 0 {
				continue
			}
			wEntry := &table[i]
			ref := refs[i]

			h := wEntry.Hash & uint64(finalMask)
			for {
				fEntry := &finalTable[h]
				if fEntry.MessageCount == 0 {
					fEntry.Hash = wEntry.Hash
					fEntry.MonthCode = wEntry.KeyMonthCode
					fEntry.ChanPtr = ref.Ptr
					fEntry.ChanLen = ref.Len
					fEntry.MinLength = wEntry.MinLength
					fEntry.MaxLength = wEntry.MaxLength
					fEntry.TotalLength = wEntry.TotalLength
					fEntry.MessageCount = wEntry.MessageCount
					fEntry.TotalStampCount = wEntry.TotalStampCount
					break
				}
				if fEntry.Hash == wEntry.Hash && fEntry.MonthCode == wEntry.KeyMonthCode {
					if wEntry.MinLength < fEntry.MinLength {
						fEntry.MinLength = wEntry.MinLength
					}
					if wEntry.MaxLength > fEntry.MaxLength {
						fEntry.MaxLength = wEntry.MaxLength
					}
					fEntry.TotalLength += wEntry.TotalLength
					fEntry.MessageCount += wEntry.MessageCount
					fEntry.TotalStampCount += wEntry.TotalStampCount
					break
				}
				h = (h + 1) & uint64(finalMask)
			}
		}
	}

	writer := bufio.NewWriterSize(outputFile, 1024*1024)
	defer writer.Flush()

	buf := make([]byte, 0, 1024)

	for i := range finalTable {
		fEntry := &finalTable[i]
		if fEntry.MessageCount == 0 {
			continue
		}
		buf = buf[:0]

		if fEntry.ChanLen > 0 {
			chanBytes := unsafe.Slice((*byte)(unsafe.Pointer(fEntry.ChanPtr)), fEntry.ChanLen)
			buf = append(buf, chanBytes...)
		}
		buf = append(buf, ',')

		m := fEntry.MonthCode % 100
		buf = append(buf, monthStrs[m]...)
		buf = append(buf, '=')

		buf = strconv.AppendUint(buf, uint64(fEntry.MinLength), 10)
		buf = append(buf, '/')

		avgLength := float64(fEntry.TotalLength) / float64(fEntry.MessageCount)
		buf = strconv.AppendFloat(buf, avgLength, 'f', 2, 64)
		buf = append(buf, '/')

		buf = strconv.AppendUint(buf, uint64(fEntry.MaxLength), 10)
		buf = append(buf, '/')

		buf = strconv.AppendUint(buf, uint64(fEntry.MessageCount), 10)
		buf = append(buf, '/')

		buf = strconv.AppendUint(buf, uint64(fEntry.TotalStampCount), 10)
		buf = append(buf, '\n')

		if _, err := writer.Write(buf); err != nil {
			fmt.Fprintf(os.Stderr, "Error writing to output file: %v\n", err)
			os.Exit(1)
		}
	}
}
