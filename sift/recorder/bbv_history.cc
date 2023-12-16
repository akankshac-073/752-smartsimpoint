#include <assert.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <limits.h>
#include <cstdlib>
#include <pin.H>
#include "globals.h"
#include "threads.h"
#include "sift/sift_format.h"
#include "sift_assert.h"
#include "sim_api.h"
using std::cerr;
using std::endl;

// Macros

#define THRESHOLD_DISTANCE 0.05
#define MAX_NUM_THREADS 64

// Global Variables

std::vector<std::vector<uint64_t>> bbvList;
std::vector<uint64_t> regionIdList;
std::vector<uint64_t> prevBbv;
std::vector<uint64_t> bbv;
std::vector<uint64_t> regionBbv;
unsigned long long thresholdDistance = THRESHOLD_DISTANCE;

VOID print_global_bbv() {

    std::cerr << "BBV: "
    for (uint64_t i = 0; i < regionBbv.size(); i++) {
        std::cerr << " " << regionBbv[i];
        if (!((i + 1) % Bbv::NUM_BBV))
            std::cerr << " |";
    }
    std::cerr << endl;
}

VOID get_global_bbv() {

    int32_t idx = 0;

    // Update previous BBV
    if (!bbv.empty())
        prevBbv = bbv;

    // Reset BBV
    if (!bbv.empty())
        bbv.clear();

    // Get global BBV
    for (idx = 0; idx < num_threads; idx++) {
        for (int dim = 0; dim < Bbv::NUM_BBV; dim++)
            bbv.push_back(thread_data[idx].bbv->getDimension(dim));
    }
    for (; idx < MAX_NUM_THREADS * Bbv::NUM_BBV; idx++)
        bbv.push_back(0);

    // Get relative global BBV for current region
    // (i.e. BBV obtained - BBV obtained until previous region)
    if (prevBbv.empty())
        regionBbv = bbv;
    else {
        for (uint64_t i = 0; i < regionBbv.size(); i++)
            regionBbv[i] = bbv[i] - prevBbv[i];
    }
}

uint64_t get_representative_region_bbv (uint64_t simRegionId) {
    long double relativeDistance = 0.0;
    long double minDistance = 1.0;
    long int nfactor = 0;
    int ret = -1;
    
    // Update global BBV list
    get_global_bbv();
    
    // If current BBV is the first BBV recorded
    // Add it to regionIdList, return BBV index (i.e., 0)
    if (regionIdList.empty()) {
        regionIdList.push_back(0);
        bbvList.push_back(regionBbv);
        ret = 0;
    }

    // Else, check if distance of current BBV is less than or equal to any
    // previously simulated regions
    else {

        // Check distance from BBVs corresponding to each entry in regionIdList
        for (uint64_t i = 0; i < regionIdList.size(); i++) {
            relativeDistance = 0.0;
        
            for (uint64_t j = 0; j < regionBbv.size(); j++) {
                auto currBbv = static_cast<int64_t>(regionBbv[j]);
                auto listBbv = static_cast<int64_t>(bbvList[i][j]);
                if (listBbv)
                    relativeDistance += (long double)(abs(listBbv - currBbv)) / (long double)(listBbv);
                else
                    nfactor++;
            }
        
            relativeDistance = relativeDistance / (MAX_NUM_THREADS * Bbv::NUM_BBV - nfactor);
        
            // If distance is less than THRESHOLD_DISTANCE
            // Update the number of regions compared, minimum distance recorded and BBV index (ret)
            if (relativeDistance < THRESHOLD_DISTANCE) {
                if (relativeDistance < minDistance) {
                    minDistance = relativeDistance;
                    ret = regionIdList[i];
                }
            }
        }
        
        // If distance from all previously simulated BBVs exceeds THRESHOLD_DISTANCE
        // Update lists, return current BBV index
        if (ret == -1) {
            regionIdList.push_back(simRegionId);
            bbvList.push_back(regionBbv);
            ret = simRegionId;
        }
    }
}

