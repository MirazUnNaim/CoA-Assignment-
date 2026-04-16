
#include <bits/stdc++.h>
using namespace std;
static const int BLOCK_WORDS = 4;   // words per cache block
static const int WORD_BYTES = 4;    // bytes per word (32-bit)
static const int BLOCK_BYTES = 16;  // bytes per block (4 words × 4 bytes)
static const int NUM_BLOCKS = 1024; // total cache lines (16 KB cache)

// Address breakdown (32-bit BYTE-addressed):
static const int OFFSET_BITS = 4; // log2(16) = 4 (byte offset in block)
static const int INDEX_BITS = 10; // log2(1024) = 10
static const int TAG_BITS = 18;   // 32 - 10 - 4 = 18

static const int MEMORY_LATENCY = 4; // cycles for memory read/write
enum class State
{
    IDLE,
    COMPARE_TAG,
    WRITE_BACK,
    ALLOCATE
};

string stateStr(State s)
{
    switch (s)
    {
    case State::IDLE:
        return "IDLE";
    case State::COMPARE_TAG:
        return "COMPARE_TAG";
    case State::WRITE_BACK:
        return "WRITE_BACK";
    case State::ALLOCATE:
        return "ALLOCATE";
    }
    return "?";
}

// Request types
enum class ReqType
{
    READ,
    WRITE
};
//  Cache Line
struct CacheLine
{
    bool valid = false;
    bool dirty = false;
    int tag = -1;
    int data[BLOCK_WORDS];
    CacheLine() { fill(data, data + BLOCK_WORDS, 0); }
};

// Byte addressed Memory model
struct Memory
{
    map<int, vector<int>> blockStore; // Key = block base address (16-byte aligned)

    void init()
    {
        // Block at byte address 0x0000: words [0, 10, 20, 30]
        blockStore[0x0000] = {0, 10, 20, 30};

        // Block at byte address 0x0010: words [40, 50, 60, 70]
        blockStore[0x0010] = {40, 50, 60, 70};

        // Block at byte address 0x0020: words [80, 90, 100, 110]
        blockStore[0x0020] = {80, 90, 100, 110};

        // Block at byte address 0x4000 (conflicts with index 0): words [200, 210, 220, 230]
        blockStore[0x4000] = {200, 210, 220, 230};

        // Testing blocks
        for (int i = 0; i < 100; i++)
        {
            int blockAddr = i * BLOCK_BYTES;
            if (blockStore.find(blockAddr) == blockStore.end())
            {
                blockStore[blockAddr] = {i * 10, i * 10 + 1, i * 10 + 2, i * 10 + 3};
            }
        }
    }

    // Get block-aligned address from byte address
    int getBlockBase(int byteAddr)
    {
        return byteAddr & ~(BLOCK_BYTES - 1); // Clear lower 4 bits
    }

    // Read an entire block (returns 4 words)
    vector<int> readBlock(int byteAddr)
    {
        int base = getBlockBase(byteAddr);
        if (blockStore.find(base) != blockStore.end())
        {
            return blockStore[base];
        }
        // If block doesn't exist, return zeros
        return vector<int>(BLOCK_WORDS, 0);
    }

    // Write an entire block to memory
    void writeBlock(int byteAddr, const int data[BLOCK_WORDS])
    {
        int base = getBlockBase(byteAddr);
        vector<int> block(BLOCK_WORDS);
        for (int i = 0; i < BLOCK_WORDS; i++)
            block[i] = data[i];
        blockStore[base] = block;
    }
};
//  CPU Request
struct CPURequest
{
    ReqType type;
    int address; // BYTE address (32-bit)
    int writeData = 0;

    string str() const
    {
        stringstream ss;
        ss << (type == ReqType::READ ? "READ " : "WRITE");
        ss << " addr=0x" << hex << setw(8) << setfill('0') << address << dec;
        if (type == ReqType::WRITE)
            ss << " data=" << writeData;
        return ss.str();
    }
};
//  Output / Interface Signals
struct Signals
{
    bool cache_hit = false;
    bool mem_read = false;
    bool mem_write = false;
    bool cpu_stall = false;
    bool done = false;
    int read_data = 0;
    int cycles_used = 0;
};
// Address Decomposition
struct AddrFields
{
    int tag;
    int index;
    int byteOffset; // byte offset within block (0-15)
    int wordOffset; // word offset within block (0-3)
};
AddrFields decompose(int byteAddr)
{
    AddrFields f;

    // Extract byte offset (bits [3:0])
    f.byteOffset = (byteAddr >> 0) & ((1 << OFFSET_BITS) - 1);

    // Extract word offset (bits [3:2] of byte address)
    f.wordOffset = (byteAddr >> 2) & 0b11;

    // Extract index (bits [13:4])
    f.index = (byteAddr >> OFFSET_BITS) & ((1 << INDEX_BITS) - 1);

    // Extract tag (bits [31:14])
    f.tag = (byteAddr >> (OFFSET_BITS + INDEX_BITS));

    return f;
}

// Block-aligned base address from a byte address
int blockBase(int byteAddr)
{
    return byteAddr & ~(BLOCK_BYTES - 1); // Clear lower 4 bits
}

// Reconstruct byte address from tag + index (for dirty eviction)
int reconstructAddr(int tag, int index)
{
    return (tag << (OFFSET_BITS + INDEX_BITS)) | (index << OFFSET_BITS);
}
//  Cache Controller FSM
class CacheController
{
    CacheLine cache[NUM_BLOCKS];
    Memory &mem;

    // Performance
    int totalHits = 0;
    int totalMisses = 0;
    int totalWriteBacks = 0;
    int totalCycles = 0;
    int requestCount = 0;

public:
    CacheController(Memory &m) : mem(m) {}

    Signals process(const CPURequest &req, ostream &log)
    {
        Signals sig;
        State state = State::IDLE;
        int cycle = 0;

        AddrFields af = decompose(req.address);
        CacheLine &line = cache[af.index];

        requestCount++;
        log << "\n"
            << string(70, '=') << "\n";
        log << "  Request #" << requestCount << " : " << req.str() << "\n";
        log << string(70, '-') << "\n";
        log << "  Address decomposition (BYTE-ADDRESSED):\n";
        log << "    Byte address     : 0x" << hex << setw(8) << setfill('0')
            << req.address << dec << " (" << req.address << ")\n";
        log << "    Tag              : 0x" << hex << af.tag << dec
            << " (" << af.tag << ")\n";
        log << "    Index            : " << af.index << "\n";
        log << "    Byte Offset      : " << af.byteOffset << "\n";
        log << "    Word Offset      : " << af.wordOffset << "\n";
        log << "    Block Base Addr  : 0x" << hex << blockBase(req.address) << dec << "\n";
        log << "  Cache line state before request:\n";
        log << "    valid=" << line.valid
            << "  dirty=" << line.dirty
            << "  tag=0x" << hex << line.tag << dec << "\n";
        log << string(70, '-') << "\n";
        //  STATE: IDLE
        log << "  Cycle " << ++cycle << " | "
            << stateStr(state) << " -> ";
        state = State::COMPARE_TAG;
        log << stateStr(state)
            << "\n    Action  : CPU request received, moving to tag comparison\n"
            << "    Signals : cpu_stall=0, valid_cpu_request=1\n";

        //  STATE: COMPARE_TAG
        log << "  Cycle " << ++cycle << " | "
            << stateStr(state) << "\n";

        bool hit = line.valid && (line.tag == af.tag);
        log << "    Check   : valid=" << line.valid
            << "  tag_in_cache=0x" << hex << line.tag
            << "  req_tag=0x" << af.tag << dec
            << "  -> " << (hit ? "HIT" : "MISS") << "\n";

        if (hit)
        {
            // CACHE HIT
            sig.cache_hit = true;
            totalHits++;

            line.valid = true;
            line.tag = af.tag;

            if (req.type == ReqType::READ)
            {
                sig.read_data = line.data[af.wordOffset];
                log << "    Action  : READ HIT — returning word from cache\n";
                log << "    Signals : cache_hit=1, cache_ready=1\n";
                log << "    Data    : word[" << af.wordOffset << "] = "
                    << sig.read_data << "\n";
            }
            else
            {
                int old = line.data[af.wordOffset];
                line.data[af.wordOffset] = req.writeData;
                line.dirty = true;
                log << "    Action  : WRITE HIT — updating cache word, marking dirty\n";
                log << "    Signals : cache_hit=1, cache_ready=1, dirty=1\n";
                log << "    Data    : word[" << af.wordOffset << "] "
                    << old << " -> " << req.writeData << "\n";
            }

            log << "  Cycle " << ++cycle << " | "
                << stateStr(state) << " -> IDLE\n";
            log << "    Action  : Cache ready, signalling done to CPU\n";
            log << "    Signals : done=1, cpu_stall=0, mark_cache_ready=1\n";
        }
        else
        {
            // CACHE MISS
            sig.cache_hit = false;
            sig.cpu_stall = true;
            totalMisses++;

            log << "    Action  : CACHE MISS — CPU stalled\n";
            log << "    Signals : cache_hit=0, cpu_stall=1\n";

            //  Dirty? WRITE_BACK first
            if (line.valid && line.dirty)
            {
                state = State::WRITE_BACK;
                int evictAddr = reconstructAddr(line.tag, af.index);

                log << "    Branch  : Old block is DIRTY → must write back first\n";
                log << "  " << string(66, '-') << "\n";
                log << "  Cycle " << cycle + 1 << "-" << cycle + MEMORY_LATENCY
                    << " | " << stateStr(state) << "\n";
                log << "    Action  : Writing dirty block to memory\n";
                log << "    Signals : mem_write=1, cpu_stall=1\n";
                log << "    Evict   : block addr=0x" << hex << evictAddr << dec
                    << " data=[";
                for (int i = 0; i < BLOCK_WORDS; i++)
                {
                    log << line.data[i];
                    if (i < BLOCK_WORDS - 1)
                        log << ", ";
                }
                log << "]\n";

                for (int c = 1; c <= MEMORY_LATENCY; c++)
                {
                    if (c < MEMORY_LATENCY)
                        log << "    Cycle " << cycle + c << " : memory_not_ready\n";
                    else
                        log << "    Cycle " << cycle + c << " : memory_ready=1 — write-back complete\n";
                }

                mem.writeBlock(evictAddr, line.data);
                sig.mem_write = true;
                totalWriteBacks++;
                cycle += MEMORY_LATENCY;

                log << "  " << string(66, '-') << "\n";
                state = State::ALLOCATE;
                log << "    Transition: WRITE_BACK -> ALLOCATE\n";
            }
            else
            {
                log << "    Branch  : Old block is CLEAN (or invalid) → go to ALLOCATE\n";
                state = State::ALLOCATE;
            }
            //  STATE: ALLOCATE
            int base = blockBase(req.address);

            log << "  " << string(66, '-') << "\n";
            log << "  Cycle " << cycle + 1 << "-" << cycle + MEMORY_LATENCY
                << " | " << stateStr(state) << "\n";
            log << "    Action  : Fetching new block from memory\n";
            log << "    Signals : mem_read=1, cpu_stall=1\n";
            log << "    Fetch   : block base addr=0x" << hex << base << dec << "\n";

            for (int c = 1; c <= MEMORY_LATENCY; c++)
            {
                if (c < MEMORY_LATENCY)
                    log << "    Cycle " << cycle + c << " : memory_not_ready\n";
                else
                    log << "    Cycle " << cycle + c << " : memory_ready=1 — allocation complete\n";
            }

            vector<int> newBlock = mem.readBlock(base);
            for (int i = 0; i < BLOCK_WORDS; i++)
                line.data[i] = newBlock[i];
            line.valid = true;
            line.dirty = false;
            line.tag = af.tag;
            sig.mem_read = true;
            cycle += MEMORY_LATENCY;

            log << "    Loaded  : [";
            for (int i = 0; i < BLOCK_WORDS; i++)
            {
                log << line.data[i];
                if (i < BLOCK_WORDS - 1)
                    log << ", ";
            }
            log << "]\n";
            log << "  " << string(66, '-') << "\n";

            state = State::COMPARE_TAG;
            log << "  Cycle " << ++cycle << " | ALLOCATE -> "
                << stateStr(state) << "\n";
            log << "    Action  : Re-evaluating after allocation\n";

            if (req.type == ReqType::WRITE)
            {
                line.data[af.wordOffset] = req.writeData;
                line.dirty = true;
                log << "    Action  : WRITE applied to newly fetched block\n";
                log << "    Signals : set_valid=1, set_tag=1, set_dirty=1\n";
                log << "    Data    : word[" << af.wordOffset << "] = " << req.writeData << "\n";
            }
            else
            {
                sig.read_data = line.data[af.wordOffset];
                log << "    Action  : READ data from newly fetched block\n";
                log << "    Signals : set_valid=1, set_tag=1\n";
                log << "    Data    : word[" << af.wordOffset << "] = " << sig.read_data << "\n";
            }

            log << "  Cycle " << ++cycle << " | "
                << stateStr(state) << " -> IDLE\n";
            log << "    Action  : Request complete, CPU unstalled\n";
            log << "    Signals : done=1, cpu_stall=0, mark_cache_ready=1\n";
        }

        sig.done = true;
        sig.cpu_stall = false;
        sig.cycles_used = cycle;
        totalCycles += cycle;

        log << string(70, '-') << "\n";
        log << "  RESULT  : " << (sig.cache_hit ? "HIT " : "MISS")
            << "  |  cycles=" << cycle
            << "  |  read_data=" << sig.read_data
            << "  |  mem_read=" << sig.mem_read
            << "  |  mem_write=" << sig.mem_write << "\n";
        log << string(70, '=') << "\n";

        return sig;
    }

    void printStats(ostream &out) const
    {
        int total = totalHits + totalMisses;
        double hitRate = total > 0 ? 100.0 * totalHits / total : 0.0;
        double missRate = total > 0 ? 100.0 * totalMisses / total : 0.0;

        out << "\n"
            << string(70, '*') << "\n";
        out << "  PERFORMANCE STATISTICS\n";
        out << string(70, '*') << "\n";
        out << "  Total Requests   : " << total << "\n";
        out << "  Cache Hits       : " << totalHits << "\n";
        out << "  Cache Misses     : " << totalMisses << "\n";
        out << "  Write-Backs      : " << totalWriteBacks << "\n";
        out << fixed << setprecision(1);
        out << "  Hit  Rate        : " << hitRate << "%\n";
        out << "  Miss Rate        : " << missRate << "%\n";
        out << "  Total Cycles     : " << totalCycles << "\n";
        out << "  Avg Cycles/Req   : "
            << (total > 0 ? (double)totalCycles / total : 0.0) << "\n";
        out << string(70, '*') << "\n";
    }
};
//  Run one scenario
void runScenario(const string &name,
                 const string &description,
                 const vector<CPURequest> &requests,
                 ostream &out)
{
    out << "\n\n"
        << string(70, '#') << "\n";
    out << "  SCENARIO : " << name << "\n";
    out << "  PURPOSE  : " << description << "\n";
    out << string(70, '#') << "\n";

    Memory mem;
    mem.init();
    CacheController ctrl(mem);

    for (const auto &req : requests)
        ctrl.process(req, out);

    ctrl.printStats(out);
}
//  Main — Test scenarios with CORRECT BYTE ADDRESSING
int main()
{
    ofstream fout("simulation_output.txt");

    struct Tee : public streambuf
    {
        streambuf *sb1, *sb2;
        Tee(streambuf *a, streambuf *b) : sb1(a), sb2(b) {}
        int overflow(int c) override
        {
            if (c == EOF)
                return !EOF;
            if (sb1->sputc(c) == EOF)
                return EOF;
            if (sb2->sputc(c) == EOF)
                return EOF;
            return c;
        }
    } tee(cout.rdbuf(), fout.rdbuf());
    ostream out(&tee);

    out << "\n"
        << string(70, '=') << "\n";
    out << "  Cache Controller FSM Simulation\n";
    out << "  Based on Patterson & Hennessy RISC-V Edition, Section 5.9\n";
    out << string(70, '-') << "\n";
    out << "  Cache Config:\n";
    out << "    Policy      : Direct-Mapped, Write-Back, Write-Allocate\n";
    out << "    Block Size  : " << BLOCK_WORDS << " words = " << BLOCK_BYTES << " bytes\n";
    out << "    Cache Size  : " << NUM_BLOCKS << " blocks (16 KB)\n";
    out << "    Address     : 32-bit BYTE-addressed\n";
    out << "    Addr Layout : [TAG=" << TAG_BITS << "b | INDEX="
        << INDEX_BITS << "b | OFFSET=" << OFFSET_BITS << "b]\n";
    out << "    Offset Split: [WORD_OFFSET=2b | BYTE_OFFSET=2b]\n";
    out << "    Mem Latency : " << MEMORY_LATENCY << " cycles\n";
    out << string(70, '=') << "\n";
    //  CORRECTED TEST SCENARIOS (BYTE-ADDRESSED)

    // S1: Cold start reads (BYTE ADDRESSES: 0, 4, 8)
    runScenario(
        "S1: Cold-Start Read Misses",
        "Three reads to different byte addresses in same block. Shows ALLOCATE path.",
        {
            {ReqType::READ, 0x0000, 0}, // word 0
            {ReqType::READ, 0x0004, 0}, // word 1
            {ReqType::READ, 0x0008, 0}, // word 2
        },
        out);

    // S2: Temporal locality
    runScenario(
        "S2: Temporal Locality",
        "Read same byte address twice, then different words in same block.",
        {
            {ReqType::READ, 0x0000, 0}, // miss
            {ReqType::READ, 0x0000, 0}, // hit (same word)
            {ReqType::READ, 0x0004, 0}, // hit (next word, byte addr +4)
            {ReqType::READ, 0x0008, 0}, // hit (byte addr +8)
        },
        out);

    // S3: Write hit
    runScenario(
        "S3: Write Hit (Dirty Bit)",
        "Load block, then write to same block. Tests dirty bit.",
        {
            {ReqType::READ, 0x0000, 0},
            {ReqType::WRITE, 0x0000, 999},
            {ReqType::READ, 0x0000, 0}, // returns 999
        },
        out);

    // S4: Write miss (write-allocate)
    runScenario(
        "S4: Write Miss (Write-Allocate)",
        "Write to empty cache. Tests write-allocate policy.",
        {
            {ReqType::WRITE, 0x0020, 777}, // new block
            {ReqType::READ, 0x0020, 0},    // returns 777
        },
        out);

    // S5: Dirty eviction
    // 0x0000 and 0x4000 map to same index (index 0)
    runScenario(
        "S5: Dirty Block Eviction (Write-Back)",
        "Write to index 0, then conflicting address. Tests WRITE_BACK path.",
        {
            {ReqType::WRITE, 0x0000, 100}, // index=0, tag=0, dirty
            {ReqType::WRITE, 0x4000, 200}, // index=0, tag=1, evicts dirty block
            {ReqType::READ, 0x4000, 0},    // hit, returns 200
        },
        out);

    // S6: Spatial locality (all 4 words in block)
    runScenario(
        "S6: Spatial Locality",
        "Access all 4 words in same block sequentially.",
        {
            {ReqType::READ, 0x0000, 0}, // miss, load block
            {ReqType::READ, 0x0004, 0}, // hit, word 1
            {ReqType::READ, 0x0008, 0}, // hit, word 2
            {ReqType::READ, 0x000C, 0}, // hit, word 3
        },
        out);

    // S7: Mixed workload
    runScenario(
        "S7: Mixed Read/Write Workload",
        "Comprehensive mix covering hits, misses, dirty eviction.",
        {
            {ReqType::READ, 0x0000, 0},
            {ReqType::WRITE, 0x0004, 555},
            {ReqType::READ, 0x0008, 0},
            {ReqType::WRITE, 0x000C, 333},
            {ReqType::READ, 0x0004, 0},    // returns 555
            {ReqType::READ, 0x000C, 0},    // returns 333
            {ReqType::WRITE, 0x4000, 888}, // conflict, dirty eviction
            {ReqType::READ, 0x4000, 0},    // returns 888
        },
        out);

    out << "\n"
        << string(70, '=') << "\n";
    out << "  Simulation complete.\n";
    out << "  Full log saved to: simulation_output.txt\n";
    out << string(70, '=') << "\n\n";

    return 0;
}
