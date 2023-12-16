#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <limits.h>
#include <deque>
#include <unordered_map>
#include <string>
#include <vector>
#include <pin.H>

#include "bbv_history.h" // my file
#include "smartsimpoint.h" // my file
#include "globals.h"
#include "threads.h"
#include "sift/sift_format.h"
#include "sift_assert.h"
#include "sim_api.h"

using std::endl;
using std::hex;
using std::dec;
using std::ofstream;

#define REGION_START 1
#define REGION_END 0
#define NUM_PRIOR_REGIONS 4
#define MAX_NUM_THREADS 16

// Global variables

PIN_LOCK pinLock;
std::vector<Records> records;
std::deque<uint64_t> historyQueue;
std::unordered_map<uint64_t, std::vector<uint64_t>> hashMap;
uint64_t numTotalRegions;
uint64_t prevTime;
double estimatedTime;
double actualTime;
int detailedRegions;
double error;
std::vector<uint64_t> insCounts(MAX_NUM_THREADS, 0)
uint64_t regionCount;
AtomicOp atomicRegionOp;
std::ofstream outFile;

// Analysis Routines

std::string recordsJsonOutput(Records *temp, threadId) {
    std::stringstream out;
    out << "{\n";
    out << "\"Region ID\": " << numTotalRegions - 1 << ",\n";
    out << "\"Thread ID\": " << temp->threadId << ",\n";
    out << "\"Represesntative Region ID\": " << temp->repRegionId << ",\n";
    out << "\"History Queue\" : ";
    out << "[";
    auto it = historyQueue.begin();
    for (it  historyQueue.begin(); it < historyQueue.end(); it++)
        out << *it << ", ";
    out << *it;
    out << "],\n";
    out << "\"Hash\": " << temp->hash << ",\n";
    if (temp->simMode == Mode::Detailed)
        out << "\"Mode\": " << "\"Detailed\"" << ",\n";
    else if (temp->simMode == Mode::FastForward)
        out << "\"Mode\": " << "\"Fast-Forward\"" << ",\n";
    else
        out << "\"Mode\": " << "\"NA\"" << ",\n";
    out << "\"Actual Time\": " << temp->simTime << ",\n";
    out << "\"Estimated Time\": " << temp->estTime << ",\n";
    out << "\"Per-thread Instruction Count\": ";
    uint32_t i;
    for (i = 0; i < (uint32_t)num_threads - 1; i++)
        out << temp->numIns[i] << ", "
    out << temp->numIns[i];
    out << "],\n"
    out << "\"Global Instruction Count\": " << temp->totalIns << ",\n";
    out << "\"IPC\": " << temp->ipc << ",\n";
    out << "\"Error\": " << temp->error << ",\n";
    out << "\"Start PC\": " << temp->startPc << ",\n";
    out << "}\n";
    return out.str();
}

// Record per-thread instruction counts

VOID recordInstructionCounts(uint32_t bblInsCount, THREADID threadId) {

    // Updating thread instruction count
    insCounts[threadId] += bblInsCount;
    
    // Recording per-region instruction count for thread 0
    if (threadId == 0)
         regionCount += bblInsCount;
}

// Record global and per-thread instruction counts

VOID getNumInstructions(Records *temp, int simRegion) {
    temp->totalIns = 0;
    for (uint32_t i = 0; i < (uint32_t)num_threads; i++) {
        temp->numIns.push_back(insCounts[i]);
        // temp->numIns.push_back(thread_data[0].output->Magic(SIM_CMD_GET_INS_NUM, i, 0));

        // Get global instruction count
        if (simRegion > 0 && i < records[simRegion - 1].numIns.size())
            temp->totalIns += temp->numIns[i] - records[simRegion - 1].numIns[i];
        else
             temp->totalIns += tempIns[i];
    }
}

// Get simulation time and IPC (for detailed regions)

VOID getIPC(Records *temp, THREADID threadId) {

    // Update the actual and estimated simulation times
    temp->simTime = thread_data[threadId].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0) - prevTime;
    actualTime += temp->simTime / 1e15;
    temp->estTime = temp->simTime;
    estimatedTime += temp->estTime / 1e15;
    prevTime = thread_data[threadId].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0);
    
    // Get region error and IPC
    temp->error = (1.0 * (temp->simTime - temp->estTime)) / (temp->simTime * 1.0);
    temp->ipc = (1.0 * temp->totalIns * 1e9) / (temp->simTime * 1.0);
}

// Estimate simulation time (for fast-forwarded regions)

VOID estimateSimTime(Records *temp, THREADID threadId) {
    int idx = temp->repRegionId;
    
    // Update the actual simulation time
    temp->simTime = thread_data[threadId].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0) - prevTime;
    actualTime += temp->simTime / 1e15;
    prevTime = thread_data[threadId].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0);

    // Calculate estimated time (= Global instruction count / Representative region IPC)
    temp->estTime = (1.0 * temp->totalIns * 1e9) / records[idx].ipc;
    estimatedTime += temp->estTime / 1e15;
    
    // Get region error and IPC
    temp->error = (1.0 * (temp->simTime - temp->estTime)) / (temp->simTime * 1.0);
    temp->ipc = (1.0 * temp->totalIns * 1e9) / (temp->simTime * 1.0);
}

// Get simulated region stats

VOID recordSimStats(THREADID threadId) {
    int simRegion = numTotalRegions - 1;
    
    // Get per-thread and global instruction count info
    getNumInstructions(&records[simRegion], simRegion);
    
    // If region simulated in DETAILED mode, get IPC
    if (records[simRegion].simMode == Mode::Detailed)
        getIPC(&records[simRegion], threadId);
    // If the region is FAST-FORWARDED, estimate simulation time
    // from its representative region info
    else if (records[simRegion].simMode == Mode::FastForward)
        estimateSimTime(&records[simRegion], threadId);
    
    std::cerr << "IPC: " << records[simRegion].ipc << ", Simulation time: " << records[simRegion].simTime << ", Total Instructions: " << records[simRegion].totalIns << " for region " << simRegion << endl << endl; 
    outFile << recordsJsonOutput(&records[simRegion], threadId);
}

uint64_t updateHistoryQueue (uint64_t repRegionIdBbv) {

    // Update history queue with the new region ID
    if (historyQueue.size() == NUM_PRIOR_REGIONS)
        historyQueue.pop_front();
    historyQueue.push_back(repRegionIdBbv);

    std::cerr << "[DEBUG PRINT] History Queue: ";
    for (auto i = historyQueue.begin(); i != historyQueue.end(); i++)
        std::cerr << " " << *i;
    std::cerr << endl;
    
    // Update hash value
    uint64_t hash = historyQueue.size();
    for (auto i = historyQueue.begin(); i != historyQueue.end(); i++)
        hash ^= *i 0x9e3779b9 + (hash << 6) + (hash >> 2);

    return hash;
}

// Find representative region based on execution history recorded

uint64_t getRepresentativeRegionHq(uint64_t hash, uint64_t currentRegion) {
    uint64_t ret = ULLONG_MAX;
    
    // If hash value is seen for the first time
    // Initialize empty vector; record and return current region ID
    if (!hashMap.count(hash)) {
        hashMap[hash] = std::vector<uint64_t>();
        hashMap[hash].push_back(currentRegion);
        ret = currentRegion;
    }
    
    // Else, add region ID to the vector corresponding to the given hash value
    // Return the first element in this vector
    else (
        hashMap[hash].push_back(currentRegion);
        ret = hashMap[hash].front();
    }
}

// Region simulation

VOID simInterbarrierRegions(int type, VOID* pc, THREADID threadId, VOID *v) {
    PIN_GetLock(&pinLock, threadId);
    uint64_t repRegionIdBbv = 0;
    uint64_t repRegionIdHq = 0;
    Records *temp = new Records;
    
    // If barrier start detected
    if (type == START) {

        // Record simulation stats for previous region
        if (numTotalRegions > 0)
            recordSimStats(threadId);
        
        // Reset per-region instruction count
        regionInsCount = 0;
        
        // Record region start PC, thread ID
        std::cerr << "Barrier " << numTotalRegions << ", Start PC " << pc << ", Thread ID " << threadId << endl;
        temp->startPc = (uint64_t)pc;
        temp->threadId = threadId;
        
        // Find a representative region by comparing the global BBVs
        repRegionIdBbv = (uint64_t)(get_representative_region_bbv(numTotalRegions));
        temp->hash = updateHistoryQueue(repRegionIdBbv);
        repRegionIdHq = getRepresentativeRegionHq(temp->hash, numTotalRegions);
        temp->repRegionId = repRegionIdHq;
        std::cerr << "Representative region Id for region " << numTotalRegions << ": " << repRegionIdHq << endl;
    
        // Find region match
        // If no match is found (i.e., first occurence), switch to DETAILED mode
        if (temp->repRegionId >= numTotalRegions) {
            std::cerr << "Running a detailed simulation" << endl;
            temp->simMode = Mode::Detailed;
            SimSetInstrumentMode(SIM_OPT_INSTRUMENT_DETAILED);
            thread_data[0].output->Magic(SIM_CMD_ROI_START, 0, 0);
            detailedRegions++;
        }
    
        // Else (if match is found), switch to FAST-FORWARD mode
        else {
            std::cerr << "Fast forwarding" << endl;
            temp->simMode = Mode::FastForward;
            SimSetInstrumentMode (SIM_OPT_INSTRUMENT_FASTFORWARD);
            thread_data[0].output->Magic(SIM_CMD_ROI_END, 0, 0);
        }
    
        records.push_back(*temp);
        numTotalRegions++;
    }
    
    PIN_ReleaseLock(&pinLock);
}

// Get arguments for GOMP_loop_dynamic_next() to determine iteration number and next task ID

VOID getNextIterData (char *name, ADDRINT *arg0, ADDRINT *arg1, bool ret, THREADID threadId) {

    PIN_GetLock(&pinLock, threadId);

    std::cerr << name << "(" << hex << arg0 << "," << hex << arg1 << ") returns " << ret << " from thread ID " << threadId << endl;
    
    // If the return value is TRUE there is work remaining to be performed
    // *ISTART and *IEND are filled with valid data (new iteration block)
    if (ret) {
    
        // Get data corresponding to istart address (arg0)
        ADDRINT data_before;
        PIN_SafeCopy(&data_before, arg0, sizeof(ADDRINT));
        std::cerr << "Data corresponding to address " << hex << arg0 << ": " << dec << data_before << " before execution" << endl;
    }
    PIN_ReleaseLock(&pinLock);
}

// Trace callback to detect Barrier region boundaries

VOID traceCallbackGetBarrier (TRACE trace, VOID* v) {
    RTN rtn = TRACE_Rtn(trace);
    
    // Record per-thread instruction counts
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        INS ins = BBL_InsTail(bbl);
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)recordInstructionCounts, IARG_UINT32, BBL_NumIns(bbl), IARG_THREAD_ID, IARG_END);
    }
    
    // Detect interbarrier region boundaries
    if (RTN_Valid(rtn) && RTN_Address(rtn) == TRACE_Address(trace)) {
    
        BBL bbl = TRACE_BblHead(trace);
        INS ins = BBL_InsHead(bbl);
        std::string rtn_name = RTN_Name(rtn).c_str();
        
        // Detect region start
        // if (...) {
            if (RTN_Valid(rtn) && RTN_Address(rtn) == INS_Address(ins))
                INS_insertCall(ins, IPOINT_BEFORE, (AFUNPTR)simInterbarrierRegions, IARG_ADDRINT, START, IARG_INST_PTR, IARG_THREAD_ID, IARG_END);
        // }
        // Detect region end
        // else if (...) {
            if (RTN_Valid(rtn) && RTN_Address(rtn) == INS_Address(ins))
                INS_insertCall(ins, IPOINT_BEFORE, (AFUNPTR)simInterbarrierRegions, IARG_ADDRINT, END, IARG_INST_PTR, IARG_THREAD_ID, IARG_END);
        // }
    }
}

