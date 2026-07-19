//! 1-BRC optimized: mmap (raw syscall) + MADV_POPULATE_READ + multi-threaded
//! SWAR parsing. Channels are interned into a small hot hash table; statistics
//! live in a flat (month-slot x channel-id) grid, so the per-line random
//! access is a single 32-byte stat update with no key comparison.
//! std-only: external crates are not allowed in this contest.
//!
//! Usage: onebrc <input.csv> <output.txt>

use std::env;
use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{self, Read};
use std::process::ExitCode;
use std::time::{Duration, Instant};

const HASH_K: u64 = 0x517c_c1b7_2722_0a95;

// ---------------------------------------------------------------------------
// Input mapping
// ---------------------------------------------------------------------------

enum InputData {
    #[cfg_attr(not(all(target_os = "linux", target_arch = "x86_64")), allow(dead_code))]
    Mapped {
        ptr: *const u8,
        len: usize,
    },
    Owned(Vec<u8>),
}

impl InputData {
    fn bytes(&self) -> &[u8] {
        match self {
            InputData::Mapped { ptr, len } => unsafe { std::slice::from_raw_parts(*ptr, *len) },
            InputData::Owned(v) => v,
        }
    }
}

/// Map the file read-only via a raw mmap syscall (std exposes no mmap wrapper
/// and external crates such as `libc` are not allowed).
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn mmap_readonly(file: &File, len: usize) -> Option<*const u8> {
    use std::os::fd::AsRawFd;
    const SYS_MMAP: usize = 9;
    const PROT_READ: usize = 0x1;
    const MAP_PRIVATE: usize = 0x2;

    let ret: usize;
    unsafe {
        std::arch::asm!(
            "syscall",
            inlateout("rax") SYS_MMAP => ret,
            in("rdi") 0usize,
            in("rsi") len,
            in("rdx") PROT_READ,
            in("r10") MAP_PRIVATE,
            in("r8") file.as_raw_fd() as usize,
            in("r9") 0usize,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack),
        );
    }
    // On error the kernel returns -errno, i.e. a value in [-4095, -1].
    if ret > usize::MAX - 4095 { None } else { Some(ret as *const u8) }
}

/// Batch-populate the page tables for `range` (readahead included). Much
/// cheaper than taking one demand fault per 4 KiB page while parsing; a
/// failure (old kernel) is harmless because demand faulting still works.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn madvise_populate(range: &[u8]) {
    const SYS_MADVISE: usize = 28;
    const MADV_POPULATE_READ: usize = 22;
    if range.is_empty() {
        return;
    }
    let addr = range.as_ptr() as usize;
    let aligned = addr & !4095;
    let len = range.len() + (addr - aligned);
    unsafe {
        std::arch::asm!(
            "syscall",
            inlateout("rax") SYS_MADVISE => _,
            in("rdi") aligned,
            in("rsi") len,
            in("rdx") MADV_POPULATE_READ,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack),
        );
    }
}

#[cfg(not(all(target_os = "linux", target_arch = "x86_64")))]
fn madvise_populate(_range: &[u8]) {}

fn open_input(path: &OsStr) -> io::Result<InputData> {
    let mut file = File::open(path)?;
    let len = file.metadata()?.len() as usize;
    if len == 0 {
        return Ok(InputData::Owned(Vec::new()));
    }
    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    if let Some(ptr) = mmap_readonly(&file, len) {
        return Ok(InputData::Mapped { ptr, len });
    }
    let mut buf = Vec::with_capacity(len);
    file.read_to_end(&mut buf)?;
    Ok(InputData::Owned(buf))
}

/// Prefetch a cache line into L1.
#[inline]
fn prefetch<T>(p: *const T) {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        use std::arch::x86_64::{_MM_HINT_T0, _mm_prefetch};
        _mm_prefetch::<_MM_HINT_T0>(p as *const i8);
    }
    #[cfg(not(target_arch = "x86_64"))]
    let _ = p;
}

// ---------------------------------------------------------------------------
// Scanning and parsing
// ---------------------------------------------------------------------------

/// Unaligned little-endian u64 load; the caller guarantees `pos + 8 <= data.len()`.
#[inline]
fn load_u64(data: &[u8], pos: usize) -> u64 {
    u64::from_le_bytes(data[pos..pos + 8].try_into().unwrap())
}

/// Position of the first `needle` at or after `pos`, or `data.len()` if absent.
#[inline]
fn find_byte(data: &[u8], mut pos: usize, needle: u8) -> usize {
    const LO: u64 = 0x0101_0101_0101_0101;
    const HI: u64 = 0x8080_8080_8080_8080;
    let rep = u64::from_ne_bytes([needle; 8]);
    while pos + 8 <= data.len() {
        let x = load_u64(data, pos) ^ rep;
        let found = x.wrapping_sub(LO) & !x & HI;
        if found != 0 {
            return pos + (found.trailing_zeros() >> 3) as usize;
        }
        pos += 8;
    }
    while pos < data.len() && data[pos] != needle {
        pos += 1;
    }
    pos
}

/// Fold one 8-byte chunk into an FxHash-style accumulator.
#[inline]
fn hash_step(h: u64, chunk: u64) -> u64 {
    (h.rotate_left(5) ^ chunk).wrapping_mul(HASH_K)
}

/// Scan the channel field starting at `start`: returns the position of the
/// terminating comma (or end of data) plus a hash of the bytes, folding
/// 8-byte chunks as it scans.
#[inline]
fn scan_channel(data: &[u8], start: usize) -> (usize, u64) {
    const LO: u64 = 0x0101_0101_0101_0101;
    const HI: u64 = 0x8080_8080_8080_8080;
    const COMMAS: u64 = 0x2c2c_2c2c_2c2c_2c2c;
    let mut h = 0u64;
    let mut pos = start;
    while pos + 8 <= data.len() {
        let chunk = load_u64(data, pos);
        let x = chunk ^ COMMAS;
        let found = x.wrapping_sub(LO) & !x & HI;
        if found != 0 {
            let n = (found.trailing_zeros() >> 3) as usize;
            if n > 0 {
                h = hash_step(h, chunk & (!0u64 >> (8 * (8 - n))));
            }
            return (pos + n, h);
        }
        h = hash_step(h, chunk);
        pos += 8;
    }
    // Fewer than 8 bytes remain: assemble the tail chunk one byte at a time.
    let mut chunk = 0u64;
    let mut n = 0;
    while pos + n < data.len() && data[pos + n] != b',' {
        chunk |= (data[pos + n] as u64) << (8 * n);
        n += 1;
    }
    if n > 0 {
        h = hash_step(h, chunk);
    }
    (pos + n, h)
}

/// Mix one logical 32-byte channel chunk. Eight position-specific odd weights
/// make the reduction order-sensitive; one scalar fold advances the chain.
#[cfg(target_arch = "x86_64")]
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn hash_avx2_chunk(
    h: u64,
    chunk: std::arch::x86_64::__m256i,
    weights: std::arch::x86_64::__m256i,
) -> u64 {
    use std::arch::x86_64::{
        _mm_cvtsi128_si32, _mm_shuffle_epi32, _mm_xor_si128, _mm256_castsi256_si128,
        _mm256_extracti128_si256, _mm256_mullo_epi32,
    };

    let products = _mm256_mullo_epi32(chunk, weights);
    let mut folded = _mm_xor_si128(
        _mm256_castsi256_si128(products),
        _mm256_extracti128_si256::<1>(products),
    );
    folded = _mm_xor_si128(folded, _mm_shuffle_epi32::<0x4e>(folded));
    folded = _mm_xor_si128(folded, _mm_shuffle_epi32::<0xb1>(folded));
    hash_step(h, _mm_cvtsi128_si32(folded) as u32 as u64)
}

#[cfg(target_arch = "x86_64")]
const fn avx2_prefix_masks() -> [u8; 64] {
    let mut masks = [0u8; 64];
    let mut i = 0;
    while i < 32 {
        masks[i] = 0xff;
        i += 1;
    }
    masks
}

#[cfg(target_arch = "x86_64")]
static AVX2_PREFIX_MASKS: [u8; 64] = avx2_prefix_masks();

/// AVX2 channel scan. Every vector load has its own whole-slice bounds proof;
/// the scalar tail preserves safety for a channel ending in the last 31 bytes.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn scan_channel_avx2(data: &[u8], start: usize) -> (usize, u64) {
    use std::arch::x86_64::{
        __m256i, _mm256_and_si256, _mm256_cmpeq_epi8, _mm256_loadu_si256,
        _mm256_movemask_epi8, _mm256_set1_epi8, _mm256_set_epi32,
    };

    let commas = _mm256_set1_epi8(b',' as i8);
    let weights = _mm256_set_epi32(
        0x9e37_79b1u32 as i32,
        0x85eb_ca77u32 as i32,
        0xc2b2_ae3du32 as i32,
        0x27d4_eb2fu32 as i32,
        0x1656_67b1u32 as i32,
        0xd3a2_646du32 as i32,
        0xfd70_46c5u32 as i32,
        0xb55a_4f09u32 as i32,
    );
    let mut h = 0x243f_6a88_85a3_08d3u64;
    let mut pos = start;
    while pos.checked_add(32).is_some_and(|limit| limit <= data.len()) {
        let chunk = unsafe { _mm256_loadu_si256(data.as_ptr().add(pos) as *const __m256i) };
        let matches = _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, commas)) as u32;
        if matches != 0 {
            let n = matches.trailing_zeros() as usize;
            if n != 0 {
                let mask = unsafe {
                    _mm256_loadu_si256(AVX2_PREFIX_MASKS.as_ptr().add(32 - n) as *const __m256i)
                };
                let masked = _mm256_and_si256(chunk, mask);
                h = unsafe { hash_avx2_chunk(h, masked, weights) };
            }
            return (pos + n, h);
        }
        h = unsafe { hash_avx2_chunk(h, chunk, weights) };
        pos += 32;
    }

    // Fewer than 32 bytes remain. Build the same zero-padded logical lanes as
    // the vector mask path, without issuing a vector load past the slice.
    let mut tail = [0u8; 32];
    let mut n = 0usize;
    while pos + n < data.len() && data[pos + n] != b',' {
        tail[n] = data[pos + n];
        n += 1;
    }
    if n != 0 {
        let chunk = unsafe { _mm256_loadu_si256(tail.as_ptr() as *const __m256i) };
        h = unsafe { hash_avx2_chunk(h, chunk, weights) };
    }
    (pos + n, h)
}

/// Hash of a full channel byte slice; must produce exactly the same value as
/// the fold done by `scan_channel` + `finalize_hash`.
fn hash_channel(channel: &[u8]) -> u64 {
    let mut h = 0u64;
    let mut i = 0;
    while i + 8 <= channel.len() {
        h = hash_step(h, load_u64(channel, i));
        i += 8;
    }
    if i < channel.len() {
        let mut chunk = 0u64;
        for (j, &b) in channel[i..].iter().enumerate() {
            chunk |= (b as u64) << (8 * j);
        }
        h = hash_step(h, chunk);
    }
    finalize_hash(h, channel.len())
}

#[inline]
fn finalize_hash(h: u64, ch_len: usize) -> u64 {
    hash_step(h, ch_len as u64)
}

/// Parse an unsigned decimal integer starting at `*pos`, stopping at the first
/// non-digit. The input is trusted to be well-formed.
#[inline]
fn parse_int(data: &[u8], pos: &mut usize) -> u64 {
    let mut v = 0u64;
    while *pos < data.len() {
        let b = data[*pos];
        if !b.is_ascii_digit() {
            break;
        }
        v = v * 10 + (b - b'0') as u64;
        *pos += 1;
    }
    v
}

const DIGIT_ZEROS: u64 = 0x3030_3030_3030_3030; // "00000000"

/// True if all 8 bytes of `chunk` are ASCII digits.
#[inline]
fn is_8_digits(chunk: u64) -> bool {
    const HI_NIB: u64 = 0xf0f0_f0f0_f0f0_f0f0;
    chunk & HI_NIB == DIGIT_ZEROS
        && (chunk.wrapping_add(0x0606_0606_0606_0606) & HI_NIB) == DIGIT_ZEROS
}

/// Byte index (0..8) of the first non-digit in `chunk`, or 8 if all digits.
#[inline]
fn first_non_digit(chunk: u64) -> usize {
    const HI_NIB: u64 = 0xf0f0_f0f0_f0f0_f0f0;
    const LO7: u64 = 0x7f7f_7f7f_7f7f_7f7f;
    let a = (chunk & HI_NIB) ^ DIGIT_ZEROS;
    let b = (chunk.wrapping_add(0x0606_0606_0606_0606) & HI_NIB) ^ DIGIT_ZEROS;
    let nd = a | b; // nonzero byte <=> non-digit
    let nz = (((nd & LO7).wrapping_add(LO7)) | nd) & 0x8080_8080_8080_8080;
    (nz.trailing_zeros() >> 3) as usize
}

/// SWAR-parse 8 ASCII digits (first digit in the lowest byte).
#[inline]
fn parse_8_digits(chunk: u64) -> u64 {
    let v = chunk - DIGIT_ZEROS;
    let v = (v.wrapping_mul(1 + (10 << 8)) >> 8) & 0x00ff_00ff_00ff_00ff;
    let v = (v.wrapping_mul(1 + (100 << 16)) >> 16) & 0x0000_ffff_0000_ffff;
    (v.wrapping_mul(1 + (10_000 << 32)) >> 32) & 0xffff_ffff
}

/// Parse a short unsigned decimal field starting at `pos`; returns the value
/// and the position of its terminating non-digit byte. Branch-light SWAR for
/// the common case (1..=7 digits with 8 readable bytes), so variable digit
/// counts do not cost one branch miss per line.
#[inline]
fn parse_field(data: &[u8], pos: usize) -> (u64, usize) {
    if pos + 8 <= data.len() {
        let chunk = load_u64(data, pos);
        let k = first_non_digit(chunk);
        if k == 0 {
            return (0, pos);
        }
        if k < 8 {
            // Right-align the k digits behind (8 - k) leading '0' bytes.
            let shifted = (chunk << (8 * (8 - k))) | (DIGIT_ZEROS >> (8 * k));
            return (parse_8_digits(shifted), pos + k);
        }
    }
    let mut p = pos;
    let v = parse_int(data, &mut p);
    (v, p)
}

/// Timestamp parser with a fast path for the common 10-digit case.
#[inline]
fn parse_ts(data: &[u8], pos: &mut usize) -> u64 {
    let p = *pos;
    if p + 11 <= data.len() && data[p + 10] == b',' {
        let chunk = load_u64(data, p);
        let d8 = data[p + 8].wrapping_sub(b'0');
        let d9 = data[p + 9].wrapping_sub(b'0');
        if is_8_digits(chunk) && d8 <= 9 && d9 <= 9 {
            *pos = p + 10;
            return parse_8_digits(chunk) * 100 + (d8 * 10 + d9) as u64;
        }
    }
    parse_int(data, pos)
}

/// Days since the Unix epoch -> (year, month) in UTC.
/// Howard Hinnant's `civil_from_days` algorithm.
fn year_month_from_days(days: i64) -> (i64, i64) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    (yoe + era * 400 + (month <= 2) as i64, month)
}

/// Precomputed day -> `year * 12 + month - 1` for days 0..65536 (through the
/// year 2149). Only ~1.5 KiB of it is touched for a one-year dataset.
fn build_ym_table() -> Vec<u32> {
    (0..1usize << 16)
        .map(|d| {
            let (y, m) = year_month_from_days(d as i64);
            (y as u32) * 12 + (m as u32 - 1)
        })
        .collect()
}

#[inline]
fn ym_of(ts: u64, ym_table: &[u32]) -> u32 {
    let day = (ts / 86_400) as usize;
    if day < ym_table.len() {
        ym_table[day]
    } else {
        let (y, m) = year_month_from_days((ts / 86_400) as i64);
        (y as u32) * 12 + (m as u32 - 1)
    }
}

/// Chunked byte-slice equality; avoids the out-of-line `bcmp` call that the
/// `==` operator produces for runtime-length slices.
#[inline]
fn bytes_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut i = 0;
    while i + 8 <= a.len() {
        if load_u64(a, i) != load_u64(b, i) {
            return false;
        }
        i += 8;
    }
    while i < a.len() {
        if a[i] != b[i] {
            return false;
        }
        i += 1;
    }
    true
}

// ---------------------------------------------------------------------------
// Channel interner: channel bytes -> dense id
// ---------------------------------------------------------------------------

/// Open-addressing map from channel to a dense id. Real datasets have few
/// distinct channels relative to rows, so this table stays cache-resident and
/// almost every lookup is a first-probe hit.
struct Interner {
    entries: Vec<InternEntry>,
    /// id -> (arena offset, byte length)
    channels: Vec<(u32, u32)>,
    arena: Vec<u8>,
    mask: usize,
}

#[derive(Clone, Copy)]
struct InternEntry {
    hash: u64,
    off: u32, // arena offset of the channel bytes
    len: u32,
    id: u32, // u32::MAX marks an empty slot
}

impl InternEntry {
    const EMPTY: InternEntry = InternEntry { hash: 0, off: 0, len: 0, id: u32::MAX };
}

impl Interner {
    const INITIAL_CAP: usize = 1 << 15;

    fn new() -> Self {
        Interner {
            entries: vec![InternEntry::EMPTY; Self::INITIAL_CAP],
            channels: Vec::new(),
            arena: Vec::new(),
            mask: Self::INITIAL_CAP - 1,
        }
    }

    #[inline]
    fn channel(&self, id: u32) -> &[u8] {
        let (off, len) = self.channels[id as usize];
        &self.arena[off as usize..off as usize + len as usize]
    }

    /// Prefetch the first probe slot for `hash`; needs no data-dependent loads,
    /// so it can be issued as soon as the hash is known.
    #[inline]
    fn prefetch_slot(&self, hash: u64) {
        prefetch(&self.entries[(hash as usize) & self.mask] as *const InternEntry);
    }

    #[inline]
    fn intern(&mut self, channel: &[u8], hash: u64) -> u32 {
        let mut idx = (hash as usize) & self.mask;
        loop {
            let e = self.entries[idx];
            if e.id != u32::MAX {
                if e.hash == hash
                    && bytes_eq(&self.arena[e.off as usize..(e.off + e.len) as usize], channel)
                {
                    return e.id;
                }
                idx = (idx + 1) & self.mask;
                continue;
            }
            return self.insert_new(idx, channel, hash);
        }
    }

    #[cold]
    fn insert_new(&mut self, idx: usize, channel: &[u8], hash: u64) -> u32 {
        let id = self.channels.len() as u32;
        let off = self.arena.len() as u32;
        let len = channel.len() as u32;
        self.channels.push((off, len));
        self.arena.extend_from_slice(channel);
        self.entries[idx] = InternEntry { hash, off, len, id };
        if (self.channels.len() + 1) * 2 > self.entries.len() {
            self.grow();
        }
        id
    }

    fn grow(&mut self) {
        let new_cap = self.entries.len() * 2;
        let new_mask = new_cap - 1;
        let mut new_entries = vec![InternEntry::EMPTY; new_cap];
        for e in &self.entries {
            if e.id != u32::MAX {
                let mut idx = (e.hash as usize) & new_mask;
                while new_entries[idx].id != u32::MAX {
                    idx = (idx + 1) & new_mask;
                }
                new_entries[idx] = *e;
            }
        }
        self.entries = new_entries;
        self.mask = new_mask;
    }

    /// Rebuild probe positions with the permanent scalar hash while preserving
    /// dense ids. Used once by the file-tail owner before scalar aggregation.
    fn rehash_for_scalar_tail(&mut self) {
        let mut new_entries = vec![InternEntry::EMPTY; self.entries.len()];
        for (id, &(off, len)) in self.channels.iter().enumerate() {
            let channel = &self.arena[off as usize..(off + len) as usize];
            let hash = hash_channel(channel);
            let mut idx = (hash as usize) & self.mask;
            while new_entries[idx].id != u32::MAX {
                idx = (idx + 1) & self.mask;
            }
            new_entries[idx] = InternEntry { hash, off, len, id: id as u32 };
        }
        self.entries = new_entries;
    }
}

// ---------------------------------------------------------------------------
// Statistics grid: (month slot, channel id) -> stats
// ---------------------------------------------------------------------------

/// Per-group statistics; `count == 0` marks an untouched cell. `min`/`max`
/// hold clamped-to-u32 message lengths (sums stay exact in u64).
#[derive(Clone, Copy, Default)]
struct Stat {
    min: u32,
    max: u32,
    sum: u64,
    count: u64,
    stamps: u64,
}

const NO_SLOT: u16 = u16::MAX;

/// Distinct months are few, so stats live in one dense array per month. Ids
/// are assigned in first-seen order, which under a skewed channel popularity
/// packs the hot channels into the front of each array.
struct Grid {
    slot_of_ym: Vec<u16>, // ym -> slot, NO_SLOT if unassigned; covers ym < 65536
    yms: Vec<u32>,        // slot -> ym
    slots: Vec<Vec<Stat>>,
}

impl Grid {
    fn new() -> Self {
        Grid { slot_of_ym: vec![NO_SLOT; 1 << 16], yms: Vec::new(), slots: Vec::new() }
    }

    #[inline]
    fn slot_for(&mut self, ym: u32) -> u32 {
        if let Some(&s) = self.slot_of_ym.get(ym as usize) {
            if s != NO_SLOT {
                return s as u32;
            }
        }
        self.slot_for_cold(ym)
    }

    /// New month, or a month index beyond the direct table (absurd years).
    #[cold]
    fn slot_for_cold(&mut self, ym: u32) -> u32 {
        if let Some(s) = self.yms.iter().position(|&y| y == ym) {
            return s as u32; // ym >= 65536, found by linear search
        }
        let s = self.yms.len();
        assert!(s < NO_SLOT as usize, "too many distinct months");
        self.yms.push(ym);
        self.slots.push(Vec::new());
        if let Some(cell) = self.slot_of_ym.get_mut(ym as usize) {
            *cell = s as u16;
        }
        s as u32
    }

    /// Make sure `slots[slot][id]` exists (so the apply loop can index it).
    #[inline]
    fn ensure(&mut self, slot: u32, id: u32) {
        let v = &mut self.slots[slot as usize];
        if (id as usize) < v.len() {
            return;
        }
        let new_len = ((id as usize + 1).next_power_of_two()).max(1024);
        v.resize(new_len, Stat::default());
    }

    #[inline]
    fn prefetch_stat(&self, slot: u32, id: u32) {
        prefetch(&self.slots[slot as usize][id as usize] as *const Stat);
    }

    #[inline]
    fn add(&mut self, slot: u32, id: u32, len: u64, stamps: u64) {
        let st = &mut self.slots[slot as usize][id as usize];
        let len32 = len.min(u32::MAX as u64) as u32;
        if st.count == 0 {
            st.min = len32;
            st.max = len32;
        } else {
            st.min = st.min.min(len32);
            st.max = st.max.max(len32);
        }
        st.sum += len;
        st.count += 1;
        st.stamps += stamps;
    }

    fn merge_stat(&mut self, slot: u32, id: u32, src: &Stat) {
        self.ensure(slot, id);
        let st = &mut self.slots[slot as usize][id as usize];
        if st.count == 0 {
            *st = *src;
        } else {
            st.min = st.min.min(src.min);
            st.max = st.max.max(src.max);
            st.sum += src.sum;
            st.count += src.count;
            st.stamps += src.stamps;
        }
    }
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

/// Lines per pipeline batch. Each batch is processed in three passes so that
/// every random memory access is prefetched roughly one pass (~a batch of
/// lines) before it is used, hiding cache/DRAM latency:
///   pass 1 parses fields and prefetches the interner slot (its address
///          depends only on the hash, not on any load);
///   pass 2 resolves channel ids (interner slot is now hot) and prefetches
///          the stat cell;
///   pass 3 applies the stat updates (stat cells are now hot).
const BATCH: usize = 128;
const AVX2_CURSORS: usize = 3;

#[derive(Clone, Copy, Default)]
struct Rec {
    hash: u64,
    ch_off: usize,
    ch_len: u32,
    ym: u32,
    len: u64,
    stamps: u64,
    id: u32,
    slot: u32,
}

/// Aggregate all lines in `data[start..end]`; both bounds are line boundaries.
#[inline(always)]
fn aggregate(data: &[u8], start: usize, end: usize, ym_table: &[u32], intern: &mut Interner, grid: &mut Grid) {
    let mut recs = [Rec::default(); BATCH];
    let mut pos = start;
    while pos < end {
        // Pass 1: parse lines, prefetch interner slots.
        let mut n = 0;
        while n < BATCH && pos < end {
            // Header, blank line, or stray text: the first field must be a digit.
            if !data[pos].is_ascii_digit() {
                pos = find_byte(data, pos, b'\n') + 1;
                continue;
            }
            let ts = parse_ts(data, &mut pos);
            pos += 1; // ','
            if pos > data.len() {
                break; // truncated final line
            }
            let ch_off = pos;
            let (comma, h) = scan_channel(data, pos);
            let ch_len = comma - ch_off;
            let hash = finalize_hash(h, ch_len);
            intern.prefetch_slot(hash);
            let ym = ym_of(ts, ym_table);

            pos = comma + 1;
            let (len, after_len) = parse_field(data, pos);
            pos = after_len + 1; // ','
            let (stamps, after_stamps) = parse_field(data, pos);
            pos = after_stamps;
            if pos < data.len() && data[pos] == b'\r' {
                pos += 1;
            }
            pos += 1; // '\n' (or one past the end on the last line)

            recs[n] = Rec { hash, ch_off, ch_len: ch_len as u32, ym, len, stamps, id: 0, slot: 0 };
            n += 1;
        }
        // Pass 2: resolve ids and month slots, prefetch stat cells.
        for r in &mut recs[..n] {
            let channel = &data[r.ch_off..r.ch_off + r.ch_len as usize];
            r.id = intern.intern(channel, r.hash);
            r.slot = grid.slot_for(r.ym);
            grid.ensure(r.slot, r.id);
            grid.prefetch_stat(r.slot, r.id);
        }
        // Pass 3: apply the updates.
        for r in &recs[..n] {
            grid.add(r.slot, r.id, r.len, r.stamps);
        }
    }
}

#[cold]
#[inline(never)]
fn aggregate_scalar_tail(
    data: &[u8],
    start: usize,
    end: usize,
    ym_table: &[u32],
    intern: &mut Interner,
    grid: &mut Grid,
) {
    aggregate(data, start, end, ym_table, intern, grid);
}

/// AVX2 aggregation variant. The fast-loop guard is against the whole input,
/// not the worker chunk: it guarantees room for the first channel load while
/// `scan_channel_avx2` checks every later 32-byte load independently.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn aggregate_avx2(
    data: &[u8],
    start: usize,
    end: usize,
    ym_table: &[u32],
    intern: &mut Interner,
    grid: &mut Grid,
) {
    let mut recs = [Rec::default(); BATCH];
    let mut boundaries = [0usize; AVX2_CURSORS + 1];
    boundaries[0] = start;
    boundaries[AVX2_CURSORS] = end;
    for (k, boundary) in boundaries.iter_mut().enumerate().take(AVX2_CURSORS).skip(1) {
        let raw = start + (end - start) * k / AVX2_CURSORS;
        *boundary = (find_byte(data, raw, b'\n') + 1).min(end);
    }
    let mut positions = [0usize; AVX2_CURSORS];
    let mut ends = [0usize; AVX2_CURSORS];
    positions.copy_from_slice(&boundaries[..AVX2_CURSORS]);
    ends.copy_from_slice(&boundaries[1..]);

    loop {
        // Pass 1: fill round-robin so independent cursor dependency chains
        // overlap, while preserving each cursor's whole-input safety guard.
        let mut n = 0;
        while n < BATCH {
            let mut progressed = false;
            for k in 0..AVX2_CURSORS {
                if n == BATCH {
                    break;
                }
                let mut pos = positions[k];
                if pos >= ends[k] || pos + 64 > data.len() {
                    continue;
                }
                progressed = true;

                // Header, blank line, or stray text: the first field must be a digit.
                if !data[pos].is_ascii_digit() {
                    positions[k] = find_byte(data, pos, b'\n') + 1;
                    continue;
                }
                let ts = parse_ts(data, &mut pos);
                pos += 1; // ','
                if pos > data.len() {
                    positions[k] = pos;
                    continue; // truncated final line
                }
                let ch_off = pos;
                let (comma, h) = unsafe { scan_channel_avx2(data, pos) };
                let ch_len = comma - ch_off;
                let hash = finalize_hash(h, ch_len);
                intern.prefetch_slot(hash);
                let ym = ym_of(ts, ym_table);

                pos = comma + 1;
                let (len, after_len) = parse_field(data, pos);
                pos = after_len + 1; // ','
                let (stamps, after_stamps) = parse_field(data, pos);
                pos = after_stamps;
                if pos < data.len() && data[pos] == b'\r' {
                    pos += 1;
                }
                pos += 1; // '\n' (or one past the end on the last line)
                positions[k] = pos;

                recs[n] = Rec {
                    hash,
                    ch_off,
                    ch_len: ch_len as u32,
                    ym,
                    len,
                    stamps,
                    id: 0,
                    slot: 0,
                };
                n += 1;
            }
            if !progressed {
                break;
            }
        }
        if n == 0 {
            break;
        }
        // Pass 2: resolve ids and month slots, prefetch stat cells.
        for r in &mut recs[..n] {
            let channel = &data[r.ch_off..r.ch_off + r.ch_len as usize];
            r.id = intern.intern(channel, r.hash);
            r.slot = grid.slot_for(r.ym);
            grid.ensure(r.slot, r.id);
            grid.prefetch_stat(r.slot, r.id);
        }
        // Pass 3: apply the updates.
        for r in &recs[..n] {
            grid.add(r.slot, r.id, r.len, r.stamps);
        }
    }

    // All AVX2 records have completed passes 2/3 before this one-way hash
    // transition. Never intern an AVX2 hash after this point.
    if (0..AVX2_CURSORS).any(|k| positions[k] < ends[k]) {
        intern.rehash_for_scalar_tail();
        for k in 0..AVX2_CURSORS {
            if positions[k] < ends[k] {
                aggregate_scalar_tail(data, positions[k], ends[k], ym_table, intern, grid);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Output rendering
// ---------------------------------------------------------------------------

#[inline]
fn push_u64(out: &mut Vec<u8>, mut v: u64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    loop {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    out.extend_from_slice(&buf[i..]);
}

/// Append `sum / count` rounded to exactly 2 decimals, bit-identical to Rust's
/// `{:.2}` float formatting (round-half-even on the exact binary value).
fn push_avg(out: &mut Vec<u8>, sum: u64, count: u64) {
    let v = sum as f64 / count as f64;
    let bits = v.to_bits();
    let exp = ((bits >> 52) & 0x7ff) as i32;
    let frac = bits & ((1u64 << 52) - 1);
    // q = round_half_even(v * 100) computed exactly from the binary value.
    let q: u128 = if exp == 0 {
        0 // v is +0 (subnormals cannot arise from sum/count here)
    } else {
        let m = (frac | (1 << 52)) as u128;
        let e = exp - 1075; // v = m * 2^e
        let num = m * 100;
        if e >= 0 {
            num << e
        } else {
            let k = (-e) as u32;
            if k >= 127 {
                0
            } else {
                let quo = num >> k;
                let rem = num & ((1u128 << k) - 1);
                let half = 1u128 << (k - 1);
                if rem > half || (rem == half && quo & 1 == 1) { quo + 1 } else { quo }
            }
        }
    };
    push_u64(out, (q / 100) as u64);
    let frac2 = (q % 100) as u8;
    out.push(b'.');
    out.push(b'0' + frac2 / 10);
    out.push(b'0' + frac2 % 10);
}

fn render_output(intern: &Interner, grid: &Grid) -> Vec<u8> {
    // At most every channel appears in every month. Reserving from that
    // contest-bounded upper limit lets us render directly from the grid,
    // without first materializing a second vector of references.
    let max_items = intern.channels.len().saturating_mul(grid.yms.len());
    let mut out = Vec::with_capacity(max_items.saturating_mul(64).saturating_add(1));
    for (slot, stats) in grid.slots.iter().enumerate() {
        let ym = grid.yms[slot];
        for (id, st) in stats.iter().enumerate() {
            if st.count != 0 {
                let ch = intern.channel(id as u32);
                let (year, month) = (ym / 12, ym % 12 + 1);
                out.extend_from_slice(ch);
                out.extend_from_slice(&[
                    b',',
                    b'0' + (year / 1000 % 10) as u8,
                    b'0' + (year / 100 % 10) as u8,
                    b'0' + (year / 10 % 10) as u8,
                    b'0' + (year % 10) as u8,
                    b'-',
                    b'0' + (month / 10) as u8,
                    b'0' + (month % 10) as u8,
                    b'=',
                ]);
                push_u64(&mut out, st.min as u64);
                out.push(b'/');
                push_avg(&mut out, st.sum, st.count);
                out.push(b'/');
                push_u64(&mut out, st.max as u64);
                out.push(b'/');
                push_u64(&mut out, st.count);
                out.push(b'/');
                push_u64(&mut out, st.stamps);
                out.push(b'\n');
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

fn n_threads(data_len: usize) -> usize {
    let hw = std::thread::available_parallelism().map_or(8, |n| n.get());
    hw.min(data_len >> 20).max(1) // keep at least ~1 MiB per thread
}

fn worker(
    data: &[u8],
    start: usize,
    end: usize,
    ym_table: &[u32],
    populate: bool,
    use_avx2: bool,
) -> (Interner, Grid, Duration, Duration) {
    let t = Instant::now();
    if populate {
        madvise_populate(&data[start..end]);
    }
    let populate_time = t.elapsed();
    let t = Instant::now();
    let mut intern = Interner::new();
    let mut grid = Grid::new();
    #[cfg(target_arch = "x86_64")]
    if use_avx2 {
        unsafe { aggregate_avx2(data, start, end, ym_table, &mut intern, &mut grid) };
    } else {
        aggregate(data, start, end, ym_table, &mut intern, &mut grid);
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        let _ = use_avx2;
        aggregate(data, start, end, ym_table, &mut intern, &mut grid);
    }
    (intern, grid, populate_time, t.elapsed())
}

fn run(input_path: &OsStr, output_path: &OsStr) -> io::Result<()> {
    let t_start = Instant::now();
    let input = open_input(input_path)?;
    let mapped = matches!(&input, InputData::Mapped { .. });
    let data = input.bytes();
    let ym_table = build_ym_table();
    #[cfg(target_arch = "x86_64")]
    let use_avx2 = std::is_x86_feature_detected!("avx2")
        && env::var_os("ONEBRC_FORCE_SCALAR").is_none();
    #[cfg(not(target_arch = "x86_64"))]
    let use_avx2 = false;
    let t_open = t_start.elapsed();

    let threads = n_threads(data.len());
    let mut cuts = Vec::with_capacity(threads + 1);
    cuts.push(0);
    for i in 1..threads {
        let raw = data.len() * i / threads;
        cuts.push((find_byte(data, raw, b'\n') + 1).min(data.len()));
    }
    cuts.push(data.len());

    let t = Instant::now();
    let results: Vec<(Interner, Grid, Duration, Duration)> = std::thread::scope(|s| {
        let handles: Vec<_> = (0..threads)
            .map(|i| {
                let (start, end) = (cuts[i], cuts[i + 1]);
                let ym_table = &ym_table;
                s.spawn(move || worker(data, start, end, ym_table, mapped, use_avx2))
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap()).collect()
    });
    let t_workers = t.elapsed();

    let t = Instant::now();
    let mut intern = Interner::new();
    let mut grid = Grid::new();
    for (src_intern, src_grid, _, _) in &results {
        for (slot, stats) in src_grid.slots.iter().enumerate() {
            let ym = src_grid.yms[slot];
            let mut dst_slot = u32::MAX; // resolved lazily per source slot
            for (id, st) in stats.iter().enumerate() {
                if st.count != 0 {
                    let ch = src_intern.channel(id as u32);
                    let gid = intern.intern(ch, hash_channel(ch));
                    if dst_slot == u32::MAX {
                        dst_slot = grid.slot_for(ym);
                    }
                    grid.merge_stat(dst_slot, gid, st);
                }
            }
        }
    }
    let t_merge = t.elapsed();

    let t = Instant::now();
    let out = render_output(&intern, &grid);
    let t_render = t.elapsed();

    let t = Instant::now();
    fs::write(output_path, &out)?;
    let t_write = t.elapsed();

    if env::var_os("ONEBRC_TIME").is_some() {
        let populate_max = results.iter().map(|r| r.2).max().unwrap_or_default();
        let parse_max = results.iter().map(|r| r.3).max().unwrap_or_default();
        eprintln!(
            "open+mmap {t_open:?} | workers {t_workers:?} (populate max {populate_max:?}, \
             parse max {parse_max:?}, threads {threads}) | merge {t_merge:?} | \
             render {t_render:?} | write {t_write:?} | total {:?}",
            t_start.elapsed()
        );
    }
    Ok(())
}

fn main() -> ExitCode {
    let args: Vec<_> = env::args_os().collect();
    let [_, input, output] = &args[..] else {
        eprintln!("usage: onebrc <input.csv> <output.txt>");
        return ExitCode::from(2);
    };
    match run(input, output) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("error: {err}");
            ExitCode::FAILURE
        }
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn sorted_lines(s: &str) -> Vec<&str> {
        let mut lines: Vec<_> = s.split('\n').filter(|line| !line.is_empty()).collect();
        lines.sort_unstable();
        lines
    }

    fn ym(ts: i64) -> (i64, i64) {
        year_month_from_days(ts.div_euclid(86_400))
    }

    #[test]
    fn month_conversion_matches_readme() {
        assert_eq!(ym(1_798_761_600), (2027, 1)); // 2027-01-01T00:00:00Z
        assert_eq!(ym(1_798_761_660), (2027, 1));
        assert_eq!(ym(1_801_440_000), (2027, 2));
    }

    #[test]
    fn month_boundaries() {
        assert_eq!(ym(0), (1970, 1));
        assert_eq!(ym(1_798_761_599), (2026, 12)); // 2026-12-31T23:59:59Z
        assert_eq!(ym(1_830_297_599), (2027, 12)); // 2027-12-31T23:59:59Z
        assert_eq!(ym(1_830_297_600), (2028, 1));
        assert_eq!(ym(1_835_395_200), (2028, 2)); // 2028-02-29T00:00:00Z (leap year)
    }

    #[test]
    fn ym_table_agrees_with_direct_computation() {
        let table = build_ym_table();
        for ts in [0u64, 86_399, 86_400, 1_798_761_600, 1_835_395_200, u32::MAX as u64] {
            let (y, m) = year_month_from_days((ts / 86_400) as i64);
            assert_eq!(ym_of(ts, &table), (y as u32) * 12 + (m as u32 - 1), "ts={ts}");
        }
    }

    #[test]
    fn find_byte_works() {
        let data = b"0123456789abcdef,xyz\n";
        assert_eq!(find_byte(data, 0, b','), 16);
        assert_eq!(find_byte(data, 17, b'\n'), 20);
        assert_eq!(find_byte(data, 0, b'#'), data.len());
        assert_eq!(find_byte(b"abc", 3, b'x'), 3);
    }

    #[test]
    fn scan_channel_matches_hash_channel() {
        for len in 1..=40 {
            let name: Vec<u8> = (0..len).map(|i| b'a' + (i % 26) as u8).collect();
            let mut buf1 = name.clone();
            buf1.extend_from_slice(b",rest");
            let mut buf2 = b"xyz".to_vec();
            buf2.extend_from_slice(&name);
            buf2.extend_from_slice(b",tail");
            let (c1, h1) = scan_channel(&buf1, 0);
            let (c2, h2) = scan_channel(&buf2, 3);
            assert_eq!(c1, len, "len={len}");
            assert_eq!(c2, 3 + len, "len={len}");
            assert_eq!(finalize_hash(h1, len), finalize_hash(h2, len), "hash differs at len={len}");
            assert_eq!(finalize_hash(h1, len), hash_channel(&name), "hash_channel differs at len={len}");
        }
        // No comma at all: stops at end of data.
        assert_eq!(scan_channel(b"abc", 0).0, 3);
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn avx2_scan_hash_is_alignment_independent() {
        if !std::is_x86_feature_detected!("avx2") {
            return;
        }
        for len in [0usize, 1, 7, 8, 15, 31, 32, 33, 48, 65] {
            let channel: Vec<u8> = (0..len).map(|i| b'a' + ((i * 11 + len) % 26) as u8).collect();
            let mut expected = None;
            for offset in 0..32 {
                let mut data = vec![b'x'; offset];
                data.extend_from_slice(&channel);
                data.extend_from_slice(b",tail-padding-for-safe-vector-loads");
                let (comma, h) = unsafe { scan_channel_avx2(&data, offset) };
                let got = (comma - offset, finalize_hash(h, comma - offset));
                assert_eq!(got.0, len, "len={len} offset={offset}");
                if let Some(want) = expected {
                    assert_eq!(got, want, "len={len} offset={offset}");
                } else {
                    expected = Some(got);
                }
            }
        }
    }

    #[test]
    fn timestamp_parsing() {
        let mut rng = 0x2545_f491_4f6c_dd1du64;
        for _ in 0..10_000 {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            let v = rng % 100_000_000_000; // covers 1..11 digit numbers
            let text = format!("{v},rest");
            let mut pos = 0;
            assert_eq!(parse_ts(text.as_bytes(), &mut pos), v, "v={v}");
            assert_eq!(text.as_bytes()[pos], b',', "v={v}");
        }
    }

    #[test]
    fn field_parsing_matches_scalar() {
        let mut rng = 0x0123_4567_89ab_cdefu64;
        for _ in 0..10_000 {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            let v = rng % 10_000_000_000; // 1..11 digits
            for term in [b",".as_ref(), b"\n".as_ref(), b"\r\n".as_ref()] {
                for pad in [b"".as_ref(), b"xyzabcdef".as_ref()] {
                    let mut buf = v.to_string().into_bytes();
                    let digits = buf.len();
                    buf.extend_from_slice(term);
                    buf.extend_from_slice(pad);
                    let (got, end) = parse_field(&buf, 0);
                    assert_eq!((got, end), (v, digits), "v={v} term={term:?} pad={pad:?}");
                }
            }
        }
        // Empty field and field at the very end of the data.
        assert_eq!(parse_field(b",x", 0), (0, 0));
        assert_eq!(parse_field(b"42", 0), (42, 2));
        assert_eq!(parse_field(b"", 0), (0, 0));
    }

    #[test]
    fn avg_formatting_matches_std() {
        let cases = [
            (0u64, 5u64),
            (801, 8),   // 100.125: exact tie, rounds to even -> 100.12
            (3, 8),     // 0.375: exact tie -> 0.38
            (1, 8),     // 0.125: exact tie -> 0.12
            (25, 1000), // 0.025 is not exact in binary -> 0.03
            (1, 3),
            (2, 3),
            (200, 2),
            (u64::MAX / 1000, 7),
        ];
        let mut rng = 0x9e37_79b9_7f4a_7c15u64;
        let check = |sum: u64, count: u64| {
            let mut out = Vec::new();
            push_avg(&mut out, sum, count);
            let expected = format!("{:.2}", sum as f64 / count as f64);
            assert_eq!(String::from_utf8(out).unwrap(), expected, "sum={sum} count={count}");
        };
        for (sum, count) in cases {
            check(sum, count);
        }
        for _ in 0..100_000 {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            let sum = rng % 1_000_000_000_000_000;
            let count = (rng >> 32) % 1_000_000 + 1;
            check(sum, count);
        }
    }

    #[test]
    fn bytes_eq_works() {
        assert!(bytes_eq(b"", b""));
        assert!(bytes_eq(b"team/dev/api", b"team/dev/api"));
        assert!(!bytes_eq(b"team/dev/api", b"team/dev/apj"));
        assert!(!bytes_eq(b"short", b"longer-name"));
        assert!(bytes_eq(b"exactly8!", b"exactly8!"));
    }

    #[test]
    fn interner_assigns_stable_ids() {
        let mut intern = Interner::new();
        let a = intern.intern(b"team/dev/api", hash_channel(b"team/dev/api"));
        let b = intern.intern(b"team/web", hash_channel(b"team/web"));
        let a2 = intern.intern(b"team/dev/api", hash_channel(b"team/dev/api"));
        assert_eq!(a, a2);
        assert_ne!(a, b);
        assert_eq!(intern.channel(a), b"team/dev/api");
        assert_eq!(intern.channel(b), b"team/web");
        // Force growth and re-check.
        for i in 0..10_000u32 {
            let name = format!("gen/{i}");
            intern.intern(name.as_bytes(), hash_channel(name.as_bytes()));
        }
        assert_eq!(intern.intern(b"team/web", hash_channel(b"team/web")), b);
    }

    fn run_on(data: &[u8]) -> String {
        let ym_table = build_ym_table();
        let mut intern = Interner::new();
        let mut grid = Grid::new();
        aggregate(data, 0, data.len(), &ym_table, &mut intern, &mut grid);
        String::from_utf8(render_output(&intern, &grid)).unwrap()
    }

    #[cfg(target_arch = "x86_64")]
    fn assert_avx2_matches_scalar(input: &[u8]) {
        let ym_table = build_ym_table();
        let mut scalar_intern = Interner::new();
        let mut scalar_grid = Grid::new();
        aggregate(
            input,
            0,
            input.len(),
            &ym_table,
            &mut scalar_intern,
            &mut scalar_grid,
        );
        let mut avx2_intern = Interner::new();
        let mut avx2_grid = Grid::new();
        unsafe {
            aggregate_avx2(
                input,
                0,
                input.len(),
                &ym_table,
                &mut avx2_intern,
                &mut avx2_grid,
            )
        };
        let scalar = String::from_utf8(render_output(&scalar_intern, &scalar_grid)).unwrap();
        let avx2 = String::from_utf8(render_output(&avx2_intern, &avx2_grid)).unwrap();
        assert_eq!(sorted_lines(&avx2), sorted_lines(&scalar));
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn avx2_cursor_subranges_match_scalar() {
        if !std::is_x86_feature_detected!("avx2") {
            return;
        }

        for lines in [0usize, 1, 2, AVX2_CURSORS - 1, AVX2_CURSORS, 2 * AVX2_CURSORS + 1] {
            for trailing_newline in [false, true] {
                let mut input = Vec::new();
                if lines == 0 && trailing_newline {
                    input.push(b'\n');
                }
                for i in 0..lines {
                    let channel = format!("tiny/{i}/{}", "x".repeat(i % 5));
                    let line = format!("18000000{i:02},{channel},{},{i}", i + 1);
                    input.extend_from_slice(line.as_bytes());
                    if i + 1 < lines || trailing_newline {
                        input.extend_from_slice(if i & 1 == 0 { b"\r\n" } else { b"\n" });
                    }
                }
                assert_avx2_matches_scalar(&input);
            }
        }

        let mut long = b"unix_timestamp,channel_path,message_length,stamp_count\n".to_vec();
        for i in 0..(3 * AVX2_CURSORS + 2) {
            let channel = format!("long/{i}/{}", "q".repeat(96 + i * 7));
            let line = format!("1800000{i:03},{channel},{},{}\n", 100 + i, i * 3);
            long.extend_from_slice(line.as_bytes());
        }
        for k in 1..AVX2_CURSORS {
            let raw = long.len() * k / AVX2_CURSORS;
            assert_ne!(long[raw], b'\n', "raw split unexpectedly landed on a newline");
        }
        assert_avx2_matches_scalar(&long);
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn avx2_matches_scalar_end_to_end() {
        if !std::is_x86_feature_detected!("avx2") {
            return;
        }

        let mut rng = 0x6a09_e667_f3bc_c909u64;
        let ts_bases = [10_000_000u64, 100_000_000, 1_800_000_000, 10_000_000_000];
        for case in 0..3usize {
            let mut input = if case == 1 {
                b"unix_timestamp,channel_path,message_length,stamp_count\r\n\r\n".to_vec()
            } else {
                b"unix_timestamp,channel_path,message_length,stamp_count\n\n".to_vec()
            };
            for local in 0..128usize {
                let i = case * 128 + local;
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                let ch_len = if i < 48 { (i * 29) % 48 + 1 } else { (rng as usize % 48) + 1 };
                let channel: Vec<u8> = (0..ch_len)
                    .map(|j| b'a' + ((j * 17 + ch_len * 5) % 26) as u8)
                    .collect();
                let ts = ts_bases[i & 3] + (rng % 10_000);
                let digits = i % 9 + 1;
                let limit = 10u64.pow(digits as u32);
                let len = (rng.rotate_left(19) % (limit - limit / 10)) + limit / 10;
                let stamps = (rng.rotate_left(37) % (limit - limit / 10)) + limit / 10;
                input.extend_from_slice(ts.to_string().as_bytes());
                input.push(b',');
                input.extend_from_slice(&channel);
                input.push(b',');
                input.extend_from_slice(len.to_string().as_bytes());
                input.push(b',');
                input.extend_from_slice(stamps.to_string().as_bytes());
                if local + 1 != 128 || case != 2 {
                    let ending = if case == 1 || (case == 2 && local % 3 == 0) {
                        b"\r\n".as_slice()
                    } else {
                        b"\n".as_slice()
                    };
                    input.extend_from_slice(ending);
                    if local % 53 == 0 {
                        input.extend_from_slice(ending);
                    }
                }
            }

            assert_avx2_matches_scalar(&input);
        }
    }

    #[test]
    fn readme_sample_end_to_end() {
        let input = b"unix_timestamp,channel_path,message_length,stamp_count\n\
            1798761600,team/dev/api,120,3\n\
            1798761660,team/dev/api,80,1\n\
            1801440000,team/dev/api,200,0\n\
            1798761720,team/web,50,2\n\
            1798761780,team/web,70,4\n";
        let expected = "team/dev/api,2027-01=80/100.00/120/2/4\n\
            team/dev/api,2027-02=200/200.00/200/1/0\n\
            team/web,2027-01=50/60.00/70/2/6\n";
        let actual = run_on(input);
        assert_eq!(sorted_lines(&actual), sorted_lines(expected));
    }

    #[test]
    fn crlf_and_missing_trailing_newline() {
        let input = b"1798761600,a,10,1\r\n1798761600,a,20,2";
        let actual = run_on(input);
        assert_eq!(sorted_lines(&actual), sorted_lines("a,2027-01=10/15.00/20/2/3\n"));
    }

    #[test]
    fn merge_combines_thread_results() {
        let ym_table = build_ym_table();
        let input = b"1798761600,team/dev/api,120,3\n\
            1798761660,team/dev/api,80,1\n\
            1801440000,team/dev/api,200,0\n\
            1798761720,team/web,50,2\n\
            1798761780,team/web,70,4\n";
        // Split at an arbitrary line boundary and aggregate the halves separately.
        let split = find_byte(input, input.len() / 2, b'\n') + 1;
        let mut parts = Vec::new();
        for (s, e) in [(0, split), (split, input.len())] {
            let mut intern = Interner::new();
            let mut grid = Grid::new();
            aggregate(input, s, e, &ym_table, &mut intern, &mut grid);
            parts.push((intern, grid));
        }
        let mut intern = Interner::new();
        let mut grid = Grid::new();
        for (src_intern, src_grid) in &parts {
            for (slot, stats) in src_grid.slots.iter().enumerate() {
                let ym = src_grid.yms[slot];
                for (id, st) in stats.iter().enumerate() {
                    if st.count != 0 {
                        let ch = src_intern.channel(id as u32);
                        let gid = intern.intern(ch, hash_channel(ch));
                        let dst_slot = grid.slot_for(ym);
                        grid.merge_stat(dst_slot, gid, st);
                    }
                }
            }
        }
        let expected = "team/dev/api,2027-01=80/100.00/120/2/4\n\
            team/dev/api,2027-02=200/200.00/200/1/0\n\
            team/web,2027-01=50/60.00/70/2/6\n";
        let actual = String::from_utf8(render_output(&intern, &grid)).unwrap();
        assert_eq!(sorted_lines(&actual), sorted_lines(expected));
    }
}
