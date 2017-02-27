/*
 * stRPHmm.c
 *
 *  Created on: Feb 4, 2017
 *      Author: benedictpaten
 */

#include "stRPHmm.h"
#include "sonLib.h"
#include <float.h>
#include <math.h>

#define ST_MATH_LOG_ZERO -INFINITY
#define ST_MATH_LOG_ONE 0.0

/*
 * Functions for manipulating read partitions described in binary
 */

uint64_t mergePartitionsOrMasks(uint64_t partition1, uint64_t partition2,
        uint64_t depthOfPartition1, uint64_t depthOfPartition2) {
    /*
     * Take two read partitions or masks and merge them together
     */
    assert(depthOfPartition1 + depthOfPartition2 <= MAX_READ_PARTITIONING_DEPTH);
    return (partition1 << depthOfPartition2) | partition2;
}

uint64_t maskPartition(uint64_t partition, uint64_t mask) {
    /*
     * Mask a read partition
     */
    return partition & mask;
}

bool seqInHap1(uint64_t partition, int64_t seqIndex) {
    /*
     * Returns non-zero if the sequence indexed by seqIndex is in the first haplotype,
     * rather than the second, according to the given partition.
     */
    assert(seqIndex < MAX_READ_PARTITIONING_DEPTH);
    return (partition >> seqIndex) & 1;
}

uint64_t makeAcceptMask(int64_t depth) {
    /*
     * Returns a mask to the given sequence depth that includes all the sequences
     */
    assert(depth <= MAX_READ_PARTITIONING_DEPTH);
    return ~(0xFFFFFFFFFFFFFFFF << depth);
}

char * intToBinaryString(uint64_t i) {
    /*
     * Converts the unsigned int to a binary string.
     */
    int64_t bits = sizeof(uint64_t);
    char * str = st_malloc((bits + 1) * sizeof(char));
    str[bits] = '\0'; //terminate the string

    // Decode the bits in from low to high in order
    // so that 14 will end up as 1110 and 15 will end up as
    // 1111 (plus some prefix bits)
    for(int64_t bit=0; bit < bits; i >>= 1) {
        str[bit++] = i & 1 ? '1' : '0';
    }

    return str;
}

/*
 * Functions to create a set of read partitioning HMMs that include a given input set of reads.
 */

int cmpint(int64_t i, int64_t j) {
    return i > j ? 1 : i < j ? -1 : 0;
}

int stRPHmm_cmpFn(const void *a, const void *b) {
    /*
     * Compares two read partitioning HMMs by coordinate on the reference.
     */
    stRPHmm *hmm1 = (stRPHmm *)a, *hmm2 = (stRPHmm *)b;
    int i = strcmp(hmm1->referenceName, hmm2->referenceName);
    if(i == 0) {
        i = cmpint(hmm1->refStart,  hmm2->refStart);
        if(i == 0) {
            i = cmpint(hmm1->refLength,  hmm2->refLength);
        }
    }
    return i;
}

stRPHmm *getNextClosestNonoverlappingHmm(stRPHmm *hmm1, stSortedSet *readHmms) {
    /*
     * Returns the HMM from the set readHmms that does not overlap hmm1
     * but whose start coordinate is closest to
     * the end coordinate of hmm1. If does not exist returns NULL.
     */

    // Iterator in the set starting from hmm1
    stSortedSetIterator *it = stSortedSet_getIteratorFrom(readHmms, hmm1);
    stRPHmm *hmm2 = stSortedSet_getNext(it);
    assert(hmm2 == hmm1);

    // For each hmm in readHmms whose coordinate is >= than hmm1's
    while((hmm2 = stSortedSet_getNext(it)) != NULL) {
        // Compare the hmm coordinates just to check that hmm2 has a coordinate >= to hmm1s
        int i = stRPHmm_cmpFn(hmm1, hmm2);
        assert(i >= 0);

        // If hmm1 and hmm2 are on different references, then hmm2 is the closest non-overlapping
        // hmm to hmm1 in reference space
        i = strcmp(hmm1->referenceName, hmm2->referenceName);
        if(i != 0) {
            break;
        }

        // If hmm2 does not overlap hmm1 it must be the closest non-overlapping hmm to hmm1
        if(hmm1->refStart + hmm1->refLength <= hmm2->refStart) {
            break;
        }
    }

    // Cleanup
    stSortedSet_destructIterator(it);

    return hmm2;
}

stSortedSet *makeComponent(stRPHmm *hmm, stSet *components) {
    /*
     * Create a component containing hmm and add the component to components.
     */
    stSortedSet *component = stSortedSet_construct3(stRPHmm_cmpFn, NULL);
    stSortedSet_insert(component, hmm);
    stSet_insert(components, component);
    return component;
}

stSet *getOverlappingComponents(stList *tilingPath1, stList *tilingPath2) {
    /*
     * Two hmms overlap if their reference coordinate intervals overlaps.
     * The transitive closure of the overlap relation
     * partitions a set of hmms into connected components.
     * This function returns this partition for the hmms in tilingPath1
     * and tilingPath2, each of which is a set of hmms sorted by reference
     * coordinate and which do not overlap in reference
     * coordinates. Each component is a stSortedSet.
     */

    // A map of hmms to components
    stHash *componentsHash = stHash_construct();

    // The set of components
    stSet *components = stSet_construct2((void (*)(void *))stSortedSet_destruct);

    // The "lagging" index of the hmm in tilingPath2 that could possibly overlap hmm1
    int64_t j = 0;

    // For each hmm in tilingPath1, in order
    for(int64_t i=0; i<stList_length(tilingPath1); i++) {
        stRPHmm *hmm1 = stList_get(tilingPath1, i);

        // Start with the component being undefined
        stSortedSet *component = NULL;

        // The "leading" index of the hmm in tilingPath2 that could possibly overlap hmm1
        int64_t k = 0;

        // While there exists an hmm in tilingPath2 that precedes or overlaps with hmm1
        while(j+k<stList_length(tilingPath2)) {

            stRPHmm *hmm2 = stList_get(tilingPath2, j+k); // Note the j+k

            // If hmm1 and hmm2 overlap
            if(stRPHmm_overlapOnReference(hmm1, hmm2)) {
                // The leading index is increased
                k++;

                // If component is still NULL
                if(component == NULL) {

                    // Look for a component for hmm2
                    component = stSet_search(components, hmm2);

                    // If hmm2 has no component make one
                    if(component == NULL) {
                        component = makeComponent(hmm2, components);
                        stHash_insert(componentsHash, hmm2, component);
                    }

                    // Add hmm1 to the component
                    stSortedSet_insert(component, hmm1);
                    stHash_insert(componentsHash, hmm1, component);
                }
                // Otherwise component is defined
                else {
                    // Add hmm2 to the component
                    assert(stHash_search(componentsHash, hmm2) == NULL); // Impossible to be defined,
                    // as implies that two
                    // hmms in tilingPath2 each both overlap two hmms in tilingPath1.
                    stSortedSet_insert(component, hmm2);
                    stHash_insert(componentsHash, hmm2, component);
                }
            }
            // Else hmm1 and hmm2 do not overlap
            else {
                assert(stRPHmm_cmpFn(hmm1, hmm2) != 0); // Not equal, obviously

                // If hmm1 occurs before hmm2 in the reference ordering
                if(stRPHmm_cmpFn(hmm1, hmm2) < 0) {

                    // If has no component, make a trivial component containing just hmm1
                    // (it doesn't overlap with any other hmm)
                    if(component == NULL) {
                        makeComponent(hmm1, components);
                    }

                    // Done with hmm1
                    break;
                }
                // else hmm2 occurs after hmm1 in the reference ordering
                else {

                    // Add hmm2 to a trivial component if it does not overlap an HMM in tiling path1
                    if(stHash_search(componentsHash, hmm2) == NULL) {
                        makeComponent(hmm2, components);
                    }

                    // Increase the lagging index as hmm1 and proceding hmms can not overlap with hmm2
                    j++;
                }

            }
        }
    }

    // For any remaining hmms in tilingPath2 that have not been placed in a component
    // put them in a component
    while(j < stList_length(tilingPath2)) {
        stRPHmm *hmm2 = stList_get(tilingPath2, j++);
        if(stHash_search(componentsHash, hmm2) == NULL) {
            makeComponent(hmm2, components);
        }
    }

    // Cleanup
    stHash_destruct(componentsHash);

    return components;
}

stList *getTilingPaths(stSortedSet *hmms) {
    /*
     * Takes set of hmms ordered by reference coordinate (see stRPHmm_cmpFn) and returns
     * a list of tiling paths. Each tiling path consisting of maximal sequences of hmms
     * that do not overlap. Destroys sortedSet in the process.
     */
    stList *tilingPaths = stList_construct();
    while(stSortedSet_size(hmms) > 0) {

        // Make an empty tiling path and add to set of tiling paths built so far
        stList *tilingPath = stList_construct();
        stList_append(tilingPaths, tilingPath);

        // Get the hmm with lowest reference coordinate and add to the tiling path
        stRPHmm *hmm = stSortedSet_getFirst(hmms);
        stList_append(tilingPath, hmm);
        stSortedSet_remove(hmms, hmm);

        // While it exists, get the next closest non-overlapping hmm
        // and add to the tiling path progressively, removing it from the
        // set of hmms left to tile
        while((hmm = getNextClosestNonoverlappingHmm(hmm, hmms)) != NULL) {
            stList_append(tilingPath, hmm);
            stSortedSet_remove(hmms, hmm);
        }
    }

    // Cleanup the input set
    stSortedSet_destruct(hmms);

    return tilingPaths;
}

stRPHmm *fuseTilingPath(stList *tilingPath) {
    /*
     * Fuse together the hmms in the tiling path into one hmm.
     * Destroys the tiling path and cleans it up.
     */
    stRPHmm *rightHmm = stList_pop(tilingPath);

    // While there remain other hmms in the list fuse them together
    while(stList_length(tilingPath) > 0) {
        stRPHmm *leftHmm = stList_pop(tilingPath);
        rightHmm = stRPHmm_fuse(leftHmm, rightHmm);
    }

    // Cleanup
    stList_destruct(tilingPath);

    return rightHmm;
}

stList *mergeTwoTilingPaths(stList *tilingPath1, stList *tilingPath2,
        double posteriorProbabilityThreshold, int64_t minColumnDepthToFilter) {
    /*
     *  Takes two lists, tilingPath1 and tilingPath2, each of which is a set of hmms
     *  ordered by reference coordinates and
     *  non-overlapping in reference coordinates.
     *  Merges together the hmms and returns a single tiling path as a result in the
     *  same format as the input lists.
     *  Destroys the input tilingPaths in the process and cleans them up.
     */

    // Partition of the hmms into overlapping connected components
    stSet *components = getOverlappingComponents(tilingPath1, tilingPath2);

    // Cleanup the input tiling paths
    stList_destruct(tilingPath1);
    stList_destruct(tilingPath2);

    // The output tiling path, which starts out empty
    stList *newTilingPath = stList_construct();

    // Fuse the hmms

    // For each component of overlapping hmms
    stList *componentsList = stSet_getList(components);
    for(int64_t i=0; i<stList_length(componentsList); i++) {
        stSortedSet *component = stList_get(componentsList, i);
        stSet_remove(components, component);

        // Make two sub-tiling paths (there can only be two maximal paths, by definition)
        stList *tilingPaths = getTilingPaths(component);

        assert(stList_length(tilingPaths) == 2);

        stList *subTilingPath1 = stList_get(tilingPaths, 0);
        stList *subTilingPath2 = stList_get(tilingPaths, 1);

        // Fuse the hmms in each sub tiling path
        stRPHmm *hmm1 = fuseTilingPath(subTilingPath1);
        stRPHmm *hmm2 = fuseTilingPath(subTilingPath2);

        // Align
        stRPHmm_alignColumns(hmm1, hmm2);

        // Merge
        stRPHmm *hmm = stRPHmm_createCrossProductOfTwoAlignedHmm(hmm1, hmm2);

        // Prune
        stRPHmm_prune(hmm, posteriorProbabilityThreshold, minColumnDepthToFilter);

        // Add to output tiling path
        stList_append(newTilingPath, hmm);
    }

    //Cleanup
    stList_destruct(componentsList);
    stSet_destruct(components);

    return newTilingPath;
}

stList *mergeTilingPaths(stList *tilingPaths, double posteriorProbabilityThreshold,
        int64_t minColumnDepthToFilter) {
    /*
     * Like mergeTwoTilingPaths(), except instead of just two tiling paths it takes a list.
     * Destroys the tiling path as it goes.
     */

    // If no tiling paths in input warn and return an empty tiling path
    if(stList_length(tilingPaths) == 0) {
        st_logCritical("WARNING: Zero tiling paths to merge\n");
        stList_destruct(tilingPaths);
        return stList_construct();
    }

    // If only one tiling path in the input, the output is just the single input tiling path
    if(stList_length(tilingPaths) == 1) {
        stList *tilingPath = stList_get(tilingPaths, 0);
        stList_destruct(tilingPaths);
        return tilingPath;
    }

    stList *tilingPath1;
    stList *tilingPath2;

    // If there are more than two tiling paths
    // split the problem into two recursively until there are just two remaining
    // tiling paths
    if(stList_length(tilingPaths) > 2) {

        // Recursively turn the first half of the tiling paths into one tiling path
        stList *tilingPaths1 = stList_construct();
        for(int64_t i=0; i<stList_length(tilingPaths)/2; i++) {
            stList_append(tilingPaths1, stList_get(tilingPaths, i));
        }
        tilingPath1 = mergeTilingPaths(tilingPaths1, posteriorProbabilityThreshold, minColumnDepthToFilter);

        // Recursively turn the other half of the tiling paths into the other tiling path
        stList *tilingPaths2 = stList_construct();
        for(int64_t i=stList_length(tilingPaths)/2; i < stList_length(tilingPaths); i++) {
            stList_append(tilingPaths2, stList_get(tilingPaths, i));
        }
        tilingPath2 = mergeTilingPaths(tilingPaths2, posteriorProbabilityThreshold, minColumnDepthToFilter);
    }
    // Otherwise the number of tiling paths is two
    else {
        tilingPath1 = stList_get(tilingPaths, 0);
        tilingPath2 = stList_get(tilingPaths, 1);
    }

    // Merge together the two tiling paths and return result
    assert(tilingPath1 != NULL);
    assert(tilingPath2 != NULL);
    stList_destruct(tilingPaths);

    return mergeTwoTilingPaths(tilingPath1, tilingPath2, posteriorProbabilityThreshold, minColumnDepthToFilter);
}

stList *getRPHmms(stList *profileSeqs, double posteriorProbabilityThreshold, int64_t minColumnDepthToFilter,
        int64_t maxCoverageDepth, double *logSubMatrix) {
    /*
     * Takes a set of profile sequences (stProfileSeq) and returns a list of read partitioning
     * hmms (stRPHmm) ordered and non-overlapping in reference coordinates.
     *
     * PosteriorProbabilityThreshold is the probability threshold used to keep cells during pruning.
     * MinColumnDepth is the size of a column to need before applying pruning.
     * MaxCoverageDepth is the maximum depth of profileSeqs to allow at any base. If the coverage depth is higher
     * than this then some profile seqs are randomly discarded.
     */

    // Create a read partitioning HMM for every sequence and put in ordered set, ordered by reference coordinate
    stSortedSet *readHmms = stSortedSet_construct3(stRPHmm_cmpFn, NULL);
    for(int64_t i=0; i<stList_length(profileSeqs); i++) {
        stSortedSet_insert(readHmms, stRPHmm_construct(stList_get(profileSeqs, i), logSubMatrix));
    }

    // Organise HMMs into "tiling paths" consisting of sequences of hmms that do not overlap
    stList *tilingPaths = getTilingPaths(readHmms);

    if(maxCoverageDepth > MAX_READ_PARTITIONING_DEPTH) {
        st_errAbort("The maximum covergae depth %" PRIi64 " is greater than the maximum allowed by the model: %"
                PRIi64 "\n", maxCoverageDepth, MAX_READ_PARTITIONING_DEPTH);
    }

    // Eliminate HMMs that cause the maximum coverage depth to exceed a threshold
    while(stList_length(tilingPaths) > maxCoverageDepth) {
        stList *tilingPath = stList_pop(tilingPaths);
        stList_destruct(tilingPath);
    }

    // Merge together the tiling paths into one merged tiling path, merging the individual hmms when
    // they overlap on the reference
    stList *finalTilingPath = mergeTilingPaths(tilingPaths, posteriorProbabilityThreshold, minColumnDepthToFilter);

    return finalTilingPath;
}

/*
 * Functions for profile sequence
 */

stProfileSeq *stProfileSeq_constructEmptyProfile(char *referenceName, int64_t referenceStart, int64_t length) {
    /*
     * Creates an empty profile sequence, with all the profile probabilities set to 0.
     */
    stProfileSeq *seq = st_malloc(sizeof(stProfileSeq));
    seq->referenceName = stString_copy(referenceName);
    seq->refStart = referenceStart;
    seq->length = length;
    seq->profileProbs = st_calloc(length, sizeof(stProfileProb));
    return seq;
}

void stProfileSeq_destruct(stProfileSeq *seq) {
    /*
     * Cleans up memory for profile sequence.
     */
    free(seq->profileProbs);
    free(seq->referenceName);
    free(seq);
}

float stProfileProb_prob(stProfileProb *p, int64_t characterIndex) {
    /*
     * Gets probability of a given character as a float.
     */
    return ((float)p->probs[characterIndex])/255;
}

void stProfileSeq_print(stProfileSeq *seq, FILE *fileHandle, bool includeSequence) {
    /*
     * Prints a debug representation of a profile sequence.
     */
    fprintf(fileHandle, "\tSEQUENCE REF_NAME: %s REF_START %"
            PRIi64 " REF_LENGTH: %" PRIi64 "\n",
            seq->referenceName, seq->refStart, seq->length);
    if(includeSequence) {
        for(int64_t i=0; i<seq->length; i++) {
            stProfileProb *p = &seq->profileProbs[i];
            fprintf(fileHandle, "\t\tPOS: %" PRIi64 " -: %f A: %f C: %f G: %f T: %f mC: %f hMC: %f mA: %f\n", i,
                    stProfileProb_prob(p, 0), stProfileProb_prob(p, 1), stProfileProb_prob(p, 2),
                    stProfileProb_prob(p, 3), stProfileProb_prob(p, 4), stProfileProb_prob(p, 5),
                    stProfileProb_prob(p, 6), stProfileProb_prob(p, 7));
        }
    }
}

/*
 * Emission probabilities
 */

/*
 * Following implement Hamming weight for uint64_t ints, taken from
 * https://en.wikipedia.org/wiki/Hamming_weight
 * TODO: Fiddle with built in popcount instruction
 */

//types and constants used in the functions below
//uint64_t is an unsigned 64-bit integer variable type (defined in C99 version of C language)
const uint64_t m1  = 0x5555555555555555; //binary: 0101...
const uint64_t m2  = 0x3333333333333333; //binary: 00110011..
const uint64_t m4  = 0x0f0f0f0f0f0f0f0f; //binary:  4 zeros,  4 ones ...
const uint64_t h01 = 0x0101010101010101; //the sum of 256 to the power of 0,1,2,3...

//This uses fewer arithmetic operations than any other known
//implementation on machines with fast multiplication.
//This algorithm uses 12 arithmetic operations, one of which is a multiply.
int popcount64(uint64_t x) {
    /*
     * Returns Hamming weight of input unsigned integer.
     */
    x -= (x >> 1) & m1;             //put count of each 2 bits into those 2 bits
    x = (x & m2) + ((x >> 2) & m2); //put count of each 4 bits into those 4 bits
    x = (x + (x >> 4)) & m4;        //put count of each 8 bits into those 8 bits
    return (x * h01) >> 56;  //returns left 8 bits of x + (x<<8) + (x<<16) + (x<<24) + ...
}

uint64_t *retrieveBitCountVector(uint64_t *bitCountVector,
        int64_t position, int64_t characterIndex, int64_t bit) {
    /*
     * Returns a pointer to a bit count vector for a given position (offset in the column),
     * character index and bit.
     */
    return &bitCountVector[position * sizeof(uint8_t) * NUCLEOTIDE_ALPHABET_SIZE + characterIndex * sizeof(uint8_t) + bit];
}

uint64_t calculateBitCountVector(stRPColumn *column,
        int64_t position, int64_t characterIndex, int64_t bit) {
    /*
     * Calculates the bit count vector for a given position, character index and bit.
     */
    uint64_t bitCountVector = 0;
    for(int64_t i=0; i<column->depth; i++) {
        stProfileProb *p = &(column->seqs[i][position]);
        bitCountVector &= (p->probs[characterIndex] >> bit) << i;
    }
    return bitCountVector;
}

uint64_t *calculateCountBitVectors(stRPColumn *column) {
    /*
     * Calculates the bit count vector for every position, character and bit in the column.
     */

    // Array of bit vectors, for each position, for each character and for each bit in uint8_t
    uint64_t *bitCountVectors = st_malloc(column->length * NUCLEOTIDE_ALPHABET_SIZE *
            NUCLEOTIDE_BITS * sizeof(uint64_t));

    // For each position
    for(int64_t i=0; i<column->length; i++) {
        // For each character
        for(int64_t j=0; j<NUCLEOTIDE_ALPHABET_SIZE; j++) {
            // For each bit
            for(int64_t k=0; k<sizeof(uint8_t); k++) {
                *retrieveBitCountVector(bitCountVectors, i, j, k) = calculateBitCountVector(column, i, j, k);
            }
        }
    }

    return bitCountVectors;
}

double getExpectedInstanceNumber(uint64_t *bitCountVectors, uint64_t depth, uint64_t partition,
        int64_t position, int64_t characterIndex) {
    /*
     * Returns the number of instances of a character, given by characterIndex, at the given position within the column for
     * the given partition.
     */
    uint64_t rawExpectedCount = 0;
    uint64_t shift = 1;
    for(int64_t i=0; i<sizeof(uint8_t); i++) {
        uint64_t j = *retrieveBitCountVector(bitCountVectors, position, characterIndex, i);
        rawExpectedCount += popcount64(j & partition) * shift;
        shift <<= 1;
    }
    double expectedCount = rawExpectedCount / ((pow(2, NUCLEOTIDE_ALPHABET_SIZE) - 1) * depth);
    assert(expectedCount >= 0.0);
    assert(expectedCount <= depth);
    return expectedCount;
}

double getSubstitutionProbability(double *subMatrix, int64_t sourceCharacterIndex,
        int64_t derivedCharacterIndex) {
    /*
     * Gets the (log) substitution probability of getting the derived character given the source (haplotype) character.
     */
    return subMatrix[sourceCharacterIndex * NUCLEOTIDE_ALPHABET_SIZE + derivedCharacterIndex];
}

double getLogProbOfReadCharacters(double *expectedInstanceNumbers, double *logSubMatrix,
        int64_t sourceCharacterIndex) {
    /*
     * Get the log probability of a given source character given the expected number of instances of each character in the reads.
     */
    double logCharacterProb = getSubstitutionProbability(logSubMatrix, sourceCharacterIndex, 0) * expectedInstanceNumbers[0];
    for(int64_t i=1; i<NUCLEOTIDE_ALPHABET_SIZE; i++) {
        logCharacterProb += getSubstitutionProbability(logSubMatrix, sourceCharacterIndex, i) *
                expectedInstanceNumbers[i];
    }
    return logCharacterProb;
}

double columnIndexLogProbability(stRPColumn *column, uint64_t index,
        uint64_t partition, uint64_t *bitCountVectors, double *logSubMatrix) {
    /*
     * Get the probability of a the characters in a given position within a column for a given partition.
     */

    // For each possible read character calculate the expected number of instances in the partition and store counts
    // in an array
    double expectedInstanceNumbers[NUCLEOTIDE_ALPHABET_SIZE];
    for(int64_t i=0; i<NUCLEOTIDE_ALPHABET_SIZE; i++) {
        expectedInstanceNumbers[i] = getExpectedInstanceNumber(bitCountVectors,
                column->depth, partition, index, i);
    }

    // Get the sum of log probabilities of the derived characters over the possible source characters
    double logColumnProb = getLogProbOfReadCharacters(expectedInstanceNumbers, logSubMatrix, 0);
    for(int64_t i=1; i<NUCLEOTIDE_ALPHABET_SIZE; i++) {
        logColumnProb = stMath_logAdd(logColumnProb, getLogProbOfReadCharacters(expectedInstanceNumbers, logSubMatrix, i));
    }

    return logColumnProb;
}

double partitionLogProbability(stRPColumn *column,
        uint64_t partition, uint64_t *bitCountVectors, double *logSubMatrix) {
    /*
     * Get the log probability of a set of reads for a given column.
     */
    assert(column->length > 0);
    double logPartitionProb = columnIndexLogProbability(column, 0,
            partition, bitCountVectors, logSubMatrix);
    for(int64_t i=1; i<column->length; i++) {
        logPartitionProb += columnIndexLogProbability(column, i,
                partition, bitCountVectors, logSubMatrix);
    }
    return logPartitionProb;
}

double emissionLogProbability(stRPColumn *column,
        stRPCell *cell, uint64_t *bitCountVectors, double *logSubMatrix) {
    /*
     * Get the log probability of a partition for a given column.
     */
    return partitionLogProbability(column, cell->partition, bitCountVectors, logSubMatrix) +
            partitionLogProbability(column, ~cell->partition, bitCountVectors, logSubMatrix);
}

/*
 * Functions for the read partitioning hmm object stRPHmm.
 */

stRPHmm *stRPHmm_construct(stProfileSeq *profileSeq, double *logSubMatrix) {
    /*
     * Create a read partitioning HMM representing the single sequence profile.
     */

    stRPHmm *hmm = st_malloc(sizeof(stRPHmm));

    //  Set reference coordinates
    hmm->referenceName = stString_copy(profileSeq->referenceName);
    hmm->refStart = profileSeq->refStart;
    hmm->refLength = profileSeq->length;

    // Add the single profile sequence to the list of the hmm's sequences
    hmm->profileSeqs = stList_construct();
    stList_append(hmm->profileSeqs, profileSeq);

    // Emission probability function
    hmm->logSubMatrix = logSubMatrix;

    hmm->columnNumber = 1; // The number of columns in the model, initially just 1
    hmm->maxDepth = 1; // The maximum number of states in a column, initially just 1

    // Create the first column of the model
    stProfileSeq **seqHeaders = st_malloc(sizeof(stProfileSeq *));
    seqHeaders[0] = profileSeq;
    stProfileProb **seqs = st_malloc(sizeof(stProfileProb *));
    seqs[0] = profileSeq->profileProbs;
    stRPColumn *column = stRPColumn_construct(hmm->refStart, hmm->refLength, 1, seqHeaders, seqs);
    hmm->firstColumn = column;
    hmm->lastColumn = column;

    // Add two cells to the column to represent the two possible partitions of the single profile sequence
    stRPCell *cell = stRPCell_construct(1);
    column->head = cell;
    cell->nCell = stRPCell_construct(0);

    return hmm;
}

void stRPHmm_destruct(stRPHmm *hmm) {
    /*
     * Free memory owned by the hmm, including columns.
     */
    free(hmm->referenceName);
    stList_destruct(hmm->profileSeqs);

    // Cleanup the columns of the hmm
    stRPColumn *column = hmm->firstColumn;
    while(1) {
        stRPMergeColumn *mColumn = column->nColumn;
        stRPColumn_destruct(column);
        if(mColumn == NULL) {
            break;
        }
        column = mColumn->nColumn;
        stRPMergeColumn_destruct(mColumn);
    }

    free(hmm);
}

stList *stRPHmm_forwardTraceBack(stRPHmm *hmm) {
    /*
     * Traces back through the forward matrix picking the most probable path.
     * (yes, this is non-symmetric)
     * Returns the result as a list of cells, one from each column.
     */
    stList *path = stList_construct();

    stRPColumn *column = hmm->lastColumn;

    // Pick cell in the last column with highest probability
    stRPCell *cell = column->head;
    double maxProb = cell->forwardLogProb;
    stRPCell *maxCell = cell;
    while((cell = cell->nCell) != NULL) {
        if(cell->forwardLogProb > maxProb) {
            maxProb = cell->forwardLogProb;
            maxCell = cell;
        }
    }

    stList_append(path, maxCell); // Add chosen cell to output

    // Walk back through previous columns
    while(column->pColumn != NULL) {
        // Get previous merge cell
        stRPMergeCell *mCell = stRPMergeColumn_getPreviousMergeCell(maxCell, column->pColumn);
        if(mCell == NULL) {
            st_errAbort("Traceback failed because a cell is missing from the HMM, is the pruning too aggressive?"
                    "Total forward prob: %f Total backward prob: %f", (float)hmm->forwardLogProb, (float)hmm->backwardLogProb);
        }

        // Switch to previous column
        column = column->pColumn->pColumn;

        // Walk through cells in the previous column to find the one with the highest forward probability that transitions
        // to maxCell
        cell = column->head;
        maxCell = NULL;
        maxProb = ST_MATH_LOG_ZERO;
        do {
            // If compatible and has greater probability
            assert(stRPMergeColumn_getNextMergeCell(cell, column->nColumn) != NULL);
            if(stRPMergeColumn_getNextMergeCell(cell, column->nColumn) == mCell && cell->forwardLogProb > maxProb) {
                maxProb = cell->forwardLogProb;
                maxCell = cell;
            }
        } while((cell = cell->nCell) != NULL);

        assert(maxCell != NULL);
        stList_append(path, maxCell);
    }

    stList_reverse(path); // So cells go in order

    return path;
}

stSet *stRPHmm_partitionSequencesByStatePath(stRPHmm *hmm, stList *path) {
    /*
     * For an hmm and path through the hmm (e.g. computed with stRPHmm_forwardTraceBack) returns the
     * set of sequences in the hmm that are predicted to come from the first haplotype path.
     */

    stSet *seqsInHap1 = stSet_construct();

    // For each cell/column pair
    stRPColumn *column = hmm->firstColumn;
    for(int64_t i=0; i<stList_length(path); i++) {
        stRPCell *cell = stList_get(path, i);

        // Get sequences in first partition
        for(int64_t j=0; j<column->depth; j++) {
            if(seqInHap1(cell->partition, j)) {
                stSet_insert(seqsInHap1, column->seqHeaders[j]);
            }
        }
    }

    return seqsInHap1;
}

void stRPHmm_print(stRPHmm *hmm, FILE *fileHandle, bool includeColumns, bool includeCells) {
    /*
     * Prints a debug friendly representation of the state of an hmm.
     */
    //Header line
    fprintf(fileHandle, "HMM REF_NAME: %s REF_START: %" PRIi64 " REF_LENGTH %" PRIi64
            " COLUMN_NUMBER %" PRIi64 " MAX_DEPTH: %" PRIi64 " FORWARD_PROB: %f BACKWARD_PROB: %f\n,",
            hmm->referenceName, hmm->refStart, hmm->refLength,
            hmm->columnNumber, hmm->maxDepth,
            (float)hmm->forwardLogProb, (float)hmm->backwardLogProb);

    if(includeColumns) {
        stRPColumn *column = hmm->firstColumn;
        int64_t i=0;
        while(1) {
            fprintf(fileHandle, "Column %" PRIi64 "\n", i++);

            // Print the column
            stRPColumn_print(column, fileHandle, includeCells);

            if(column->nColumn == NULL) {
                break;
            }

            // Print the merge column
            stRPMergeColumn_print(column->nColumn, fileHandle, includeCells);

            column = column->nColumn->nColumn;
        }
    }
}

stRPHmm *stRPHmm_fuse(stRPHmm *leftHmm, stRPHmm *rightHmm) {
    /*
     * Fuses together two hmms, such that leftHmm and rightHMM are on the same reference sequence and non-overlapping and
     * left hmm preceds right hmm on the reference sequence.
     * Returns fused hmm, destroys input hmms in the process.
     */

    // Checks
    if(!stString_eq(leftHmm->referenceName, rightHmm->referenceName)) {
        st_errAbort("Attempting to fuse two hmms not on the same reference sequence");
    }

    if(stRPHmm_overlapOnReference(leftHmm, rightHmm)) {
        st_errAbort("Attemping to fuse two hmms that overlap in reference coordinates");
    }

    if(leftHmm->refStart >= rightHmm->refStart) {
        st_errAbort("Left hmm does not precede right hmm in reference coordinates for merge");
    }

    // Create a new empty hmm
    stRPHmm *hmm = st_malloc(sizeof(stRPHmm));
    // Set the reference interval
    hmm->referenceName = stString_copy(leftHmm->referenceName);
    hmm->refStart = leftHmm->refStart;
    hmm->refLength = rightHmm->refStart + rightHmm->refLength - leftHmm->refStart;
    // Create the combined list of profile seqs
    hmm->profileSeqs = stList_copy(leftHmm->profileSeqs, NULL);
    stList_appendAll(hmm->profileSeqs, rightHmm->profileSeqs);
    // Set column number
    hmm->columnNumber = leftHmm->columnNumber + rightHmm->columnNumber;
    // Max depth
    hmm->maxDepth = leftHmm->maxDepth > rightHmm->maxDepth ? leftHmm->maxDepth : rightHmm->maxDepth;
    // Emission prob
    if(leftHmm->logSubMatrix != rightHmm->logSubMatrix) {
        st_errAbort("Substitution matrices differ in fuse function, panic.");
    }
    hmm->logSubMatrix = leftHmm->logSubMatrix;

    // Make columns to fuse left hmm and right hmm's columns
    stRPMergeColumn *mColumn = stRPMergeColumn_construct(0, 0);
    leftHmm->lastColumn->nColumn = mColumn;
    mColumn->pColumn = leftHmm->lastColumn;
    int64_t gapLength = rightHmm->refStart - leftHmm->refStart + leftHmm->refLength;
    assert(gapLength >= 0);
    if(gapLength > 0) {
        stRPColumn *column = stRPColumn_construct(leftHmm->refStart + leftHmm->refLength,
                gapLength, 0, NULL, NULL);
        mColumn->nColumn = column;
        column->pColumn = mColumn;
        column->head = stRPCell_construct(0); // Make cell for empty column
        mColumn = stRPMergeColumn_construct(0, 0);
        column->nColumn = mColumn;
        mColumn->pColumn = column;
        // Increase the column number to account for the introduced gap column
        hmm->columnNumber += 1;
    }
    mColumn->nColumn = rightHmm->firstColumn;
    rightHmm->firstColumn->pColumn = mColumn;

    // Initialise first/last columns of fused hmm
    hmm->firstColumn = leftHmm->firstColumn;
    hmm->lastColumn = rightHmm->lastColumn;

    // Cleanup
    stRPHmm_destruct(leftHmm);
    stRPHmm_destruct(rightHmm);

    return hmm;
}

void stRPHmm_alignColumns(stRPHmm *hmm1, stRPHmm *hmm2) {
    /*
     * Align the input hmms, modifying them in place, so that they each
     *  (1) span the same reference interval,
     *  (2) have the same number of columns, and
     *  (3) so that for all i, column i in each model span the same interval.
     */

    // If the two hmms don't overlap in reference space then complain
    if(!stRPHmm_overlapOnReference(hmm1, hmm2)) {
        st_errAbort("Attempting to align two HMMs that do not overlap in reference coordinate space");
    }

    // If hmm1 starts after hmm2 then call the other way around
    if(hmm1->refStart > hmm2->refStart) {
        stRPHmm_alignColumns(hmm2, hmm1);
        return;
    }

    // If hmm1 starts before hmm2 add an empty prefix interval to hmm2
    // so they have the same start coordinate
    if(hmm1->refStart < hmm2->refStart) {
        // Create column
        stRPColumn *column = stRPColumn_construct(hmm1->refStart, hmm2->refStart - hmm1->refStart,
                0, NULL, NULL);
        // Add cell
        column->head = stRPCell_construct(0);
        // Create merge column
        stRPMergeColumn *mColumn = stRPMergeColumn_construct(0,0);
        // Add merge cell
        stRPMergeCell_construct(0, 0, mColumn);
        // Create links
        hmm2->firstColumn->pColumn = mColumn;
        mColumn->nColumn = hmm2->firstColumn;
        mColumn->pColumn = column;
        column->nColumn = mColumn;
        hmm2->firstColumn = column;
        //Adjust start and length of hmm2 interval
        hmm2->refLength += hmm2->refStart - hmm1->refStart;
        hmm2->refStart = hmm1->refStart;
        // Increase column number
        hmm2->columnNumber++;
    }

    // If hmm1 has a shorter reference interval length than hmm2 then call the function
    // with the hmms reversed.
    if(hmm1->refLength < hmm2->refLength) {
        stRPHmm_alignColumns(hmm2, hmm1);
        return;
    }

    // If hmm1 has a longer reference interval than hmm2 append an empty suffix
    // interval to hmm2 to make them the same length.
    if(hmm1->refLength > hmm2->refLength) {
        // Create column
        stRPColumn *column = stRPColumn_construct(hmm1->lastColumn->refStart + hmm1->lastColumn->length,
                hmm1->refLength - hmm2->refLength, 0, NULL, NULL);
        // Add cell
        column->head = stRPCell_construct(0);
        // Create merge column
        stRPMergeColumn *mColumn = stRPMergeColumn_construct(0, 0);
        // Add merge cell
        stRPMergeCell_construct(0, 0, mColumn);
        // Create links
        hmm2->lastColumn->nColumn = mColumn;
        mColumn->pColumn = hmm2->lastColumn;
        mColumn->nColumn = column;
        column->pColumn = mColumn;
        hmm2->lastColumn = column;
        //Adjust start and length of hmm2 interval
        hmm2->refLength = hmm1->refLength;
        // Increase column number
        hmm2->columnNumber++;
    }

    // At this point both hmms have the same reference interval

    // While one hmm has a shorter reference interval than the other split the other interval
    // otherwise move on to the next
    stRPColumn *column1 = hmm1->firstColumn;
    stRPColumn *column2 = hmm2->firstColumn;
    while(1) {
        assert(column1->refStart == column2->refStart);

        if(column1->length > column2->length) {
            stRPColumn_split(column1, column2->length, hmm1);
        }
        else if(column1->length < column2->length) {
            stRPColumn_split(column2, column1->length, hmm2);
        }

        // There are no more columns, so break
        if(column1->nColumn == NULL) {
            assert(column2->nColumn == NULL);
            break;
        }

        column1 = column1->nColumn->nColumn;
        assert(column2->nColumn != NULL);
        column2 = column2->nColumn->nColumn;
        assert(column1 != NULL);
        assert(column2 != NULL);
    }

    assert(hmm1->columnNumber == hmm2->columnNumber);
}

stRPHmm *stRPHmm_createCrossProductOfTwoAlignedHmm(stRPHmm *hmm1, stRPHmm *hmm2) {
    /*
     *  For two aligned hmms (see stRPHmm_alignColumns) returns a new hmm that represents the
     *  cross product of all the states of the two input hmms.
     */

    // If the two hmms have not been previously aligned
    if(stRPHmm_cmpFn(hmm1, hmm2) != 0 || hmm1->columnNumber != hmm2->columnNumber) {
        st_errAbort("Trying to create cross product of two unalignd HMMs");
    }

    // Create a new empty hmm
    stRPHmm *hmm = st_calloc(1, sizeof(stRPHmm));
    // Set the reference interval
    hmm->referenceName = stString_copy(hmm1->referenceName);
    hmm->refStart = hmm1->refStart;
    hmm->refLength = hmm1->refLength;
    // Create the combined list of profile seqs
    hmm->profileSeqs = stList_copy(hmm1->profileSeqs, NULL);
    stList_appendAll(hmm->profileSeqs, hmm2->profileSeqs);
    // Set column number
    hmm->columnNumber = hmm1->columnNumber;
    // Set emission function
    if(hmm1->logSubMatrix != hmm2->logSubMatrix) {
        st_errAbort("Log subsitution matrix functions differ, panic.");
    }
    hmm->logSubMatrix = hmm1->logSubMatrix;

    // For each pair of corresponding columns
    stRPColumn *column1 = hmm1->firstColumn;
    stRPColumn *column2 = hmm2->firstColumn;
    assert(column1 != NULL);
    assert(column2 != NULL);
    stRPMergeColumn *mColumn = NULL;

    while(1) {
        // Check columns aligned
        assert(column1->refStart == column2->refStart);
        assert(column1->length == column2->length);

        // Create the new column

        // Depth
        int64_t newColumnDepth = column1->depth+column2->depth;
        if(newColumnDepth > hmm->maxDepth) {
            hmm->maxDepth = newColumnDepth;
        }

        // Seq headers
        stProfileSeq **seqHeaders = st_malloc(sizeof(stProfileSeq *) * newColumnDepth);
        memcpy(seqHeaders, column1->seqHeaders, sizeof(stProfileSeq *) * column1->depth);
        memcpy(&seqHeaders[column1->depth], column2->seqs, sizeof(stProfileSeq *) * column2->depth);

        // Profiles
        stProfileProb **seqs = st_malloc(sizeof(stProfileProb *) * newColumnDepth);
        memcpy(seqs, column1->seqs, sizeof(stProfileProb *) * column1->depth);
        memcpy(&seqs[column1->depth], column2->seqs, sizeof(stProfileProb *) * column2->depth);

        stRPColumn *column = stRPColumn_construct(column1->refStart, column1->length,
                newColumnDepth, seqHeaders, seqs);

        // If the there is a previous column
        if(mColumn != NULL) {
            mColumn->nColumn = column;
            column->pColumn = mColumn;
        }
        else {
            hmm->firstColumn = column;
        }

        // Create cross product of columns
        stRPCell **pCell = &(column->head);
        stRPCell *cell1 = column1->head;
        do {
            stRPCell *cell2 = column2->head;
            do {
                stRPCell *cell = stRPCell_construct(mergePartitionsOrMasks(cell1->partition, cell2->partition,
                        column1->depth, column2->depth));
                // Link cells
                *pCell = cell;
                pCell = &cell;
            } while((cell2 = cell2->nCell) != NULL);
        } while((cell1 = cell1->nCell) != NULL);

        // Get the next merged column
        stRPMergeColumn *mColumn1 = column1->nColumn;
        stRPMergeColumn *mColumn2 = column2->nColumn;

        // If column is NULL, we have reached the last column
        // and we can exit
        if(mColumn1 == NULL) {
            assert(mColumn2 == NULL);

            // Set the last column pointer
            hmm->lastColumn = column;
            break;
        }

        // Create merged column
        uint64_t fromMask = mergePartitionsOrMasks(mColumn1->maskFrom, mColumn2->maskFrom,
                mColumn1->pColumn->depth, mColumn2->pColumn->depth);
        uint64_t toMask = mergePartitionsOrMasks(mColumn1->maskTo, mColumn2->maskTo,
                        mColumn1->nColumn->depth, mColumn2->nColumn->depth);
        mColumn = stRPMergeColumn_construct(fromMask, toMask);
        mColumn->pColumn = column;

        // Create cross product of merged columns
        stHashIterator *cellIt1 = stHash_getIterator(mColumn1->mergeCellsFrom);
        stRPMergeCell *mCell1;
        while((mCell1 = stHash_getNext(cellIt1)) != NULL) {
            stHashIterator *cellIt2 = stHash_getIterator(mColumn2->mergeCellsFrom);
            stRPMergeCell *mCell2;
            while((mCell2 = stHash_getNext(cellIt2)) != NULL) {
                uint64_t fromPartition = mergePartitionsOrMasks(mCell1->fromPartition, mCell2->fromPartition,
                        mColumn1->pColumn->depth, mColumn2->pColumn->depth);

                uint64_t toPartition = mergePartitionsOrMasks(mCell1->toPartition, mCell2->toPartition,
                        mColumn1->nColumn->depth, mColumn2->nColumn->depth);

                stRPMergeCell_construct(fromPartition, toPartition, mColumn);
            }
            stHash_destructIterator(cellIt2);
        }
        stHash_destructIterator(cellIt1);

        // Get next column
        column1 = mColumn1->nColumn;
        column2 = mColumn2->nColumn;
        assert(column1 != NULL);
        assert(column2 != NULL);
    }

    return hmm;
}

void stRPHmm_initialiseForwardProbs(stRPHmm *hmm) {
    /*
     * Initialize the forward matrix.
     */
    // Initialise total forward probability
    hmm->forwardLogProb = ST_MATH_LOG_ZERO;

    // Iterate through columns from first to last
    stRPColumn *column = hmm->firstColumn;
    while(1) {
        // Initialize column probability
        column->forwardLogProb = ST_MATH_LOG_ZERO;

        // Initialise cells in the column
        stRPCell *cell = column->head;
        do {
            cell->forwardLogProb = ST_MATH_LOG_ZERO;
        } while((cell = cell->nCell) != NULL);

        if(column->nColumn == NULL) {
            break;
        }

        // Initialise cells in the next merge column
        stList *mergeCells = stHash_getValues(column->nColumn->mergeCellsFrom);
        for(int64_t i=0; i<stList_length(mergeCells); i++) {
            stRPMergeCell *mergeCell = stList_get(mergeCells, i);
            mergeCell->forwardLogProb = ST_MATH_LOG_ZERO;
        }
        stList_destruct(mergeCells);

        column = column->nColumn->nColumn;
    }
}

void stRPHmm_forward(stRPHmm *hmm) {
    /*
     * Forward algorithm for hmm.
     */
    // Initialise state values
    stRPHmm_initialiseForwardProbs(hmm);

    stRPColumn *column = hmm->firstColumn;

    // Iterate through columns from first to last
    while(1) {
        // Get the bit count vectors for the column
        uint64_t *bitCountVectors = calculateCountBitVectors(column);

        // Iterate through states in column
        stRPCell *cell = column->head;
        do {
            // If the previous merge column exists then propagate forward probability from merge state
            if(column->pColumn != NULL) {
                stRPMergeCell *mCell = stRPMergeColumn_getPreviousMergeCell(cell, column->pColumn);
                if(mCell != NULL) { // Cell could be missing if previously pruned out
                    cell->forwardLogProb = mCell->forwardLogProb;
                }
            }
            // Otherwise initialize probability with log(1.0)
            else {
                cell->forwardLogProb = ST_MATH_LOG_ONE;
            }

            // Emission prob
            cell->forwardLogProb += emissionLogProbability(column, cell, bitCountVectors, hmm->logSubMatrix);

            // If the next merge column exists then propagate forward probability to the merge state
            if(column->nColumn != NULL) {
                // Add to the next merge cell
                stRPMergeCell *mCell = stRPMergeColumn_getNextMergeCell(cell, column->nColumn);
                if(mCell != NULL) { // Cell could be missing if previously pruned out
                    mCell->forwardLogProb = stMath_logAdd(cell->forwardLogProb, mCell->forwardLogProb);
                }
            }
            else {
                // Else propagate probability to total forward probability of model
                hmm->forwardLogProb = stMath_logAdd(hmm->forwardLogProb, cell->forwardLogProb);
            }

            // Add to column forward probability
            column->forwardLogProb = stMath_logAdd(column->forwardLogProb, cell->forwardLogProb);
        }
        while((cell = cell->nCell) != NULL);

        // Cleanup the bit count vectors
        free(bitCountVectors);

        if(column->nColumn == NULL) {
            break;
        }
        column = column->nColumn->nColumn;
    }
}

void stRPHmm_initialiseBackwardProbs(stRPHmm *hmm) {
    /*
     * Initialize the backward matrix.
     */
    // Initialize total backward probability
    hmm->backwardLogProb = ST_MATH_LOG_ZERO;

    // Iterate through columns
    stRPColumn *column = hmm->firstColumn;
    while(1) {
        // Initialize column probability
        column->backwardLogProb = ST_MATH_LOG_ZERO;

        // Initialize cells in the column
        stRPCell *cell = column->head;
        do {
            cell->backwardLogProb = ST_MATH_LOG_ZERO;
        } while((cell = cell->nCell) != NULL);

        if(column->nColumn == NULL) {
            break;
        }

        // Initialize cells in the next merge column
        stList *mergeCells = stHash_getValues(column->nColumn->mergeCellsFrom);
        for(int64_t i=0; i<stList_length(mergeCells); i++) {
            stRPMergeCell *mergeCell = stList_get(mergeCells, i);
            mergeCell->backwardLogProb = ST_MATH_LOG_ZERO;
        }
        stList_destruct(mergeCells);

        column = column->nColumn->nColumn;
    }
}

void stRPHmm_backward(stRPHmm *hmm) {
    /*
     * Backward algorithm for hmm.
     */
    stRPColumn *column = hmm->lastColumn;

    // Initialize backward probabilities
    stRPHmm_initialiseBackwardProbs(hmm);

    // Iterate through columns from last to first
    while(1) {
        // Get the bit count vectors for the column
        uint64_t *bitCountVectors = calculateCountBitVectors(column);

        // Iterate through states in column
        stRPCell *cell = column->head;
        do {
            // If the next merge column exists then propagate backward probability from merge state
            if(column->nColumn != NULL) {
                stRPMergeCell *mCell = stRPMergeColumn_getNextMergeCell(cell, column->nColumn);
                if(mCell != NULL) { // Cell could be missing if previously pruned out
                    cell->backwardLogProb = mCell->backwardLogProb;
                }
            }
            // Otherwise initialize probability with log(1.0)
            else {
                cell->backwardLogProb = ST_MATH_LOG_ONE;
            }

            // Total backward prob to propagate
            double backwardLogProb = cell->backwardLogProb + emissionLogProbability(column, cell,
                    bitCountVectors, hmm->logSubMatrix);

            // If the previous merge column exists then propagate backward probability to the merge state
            if(column->pColumn != NULL) {
                // Add to the previous merge cell
                stRPMergeCell *mCell = stRPMergeColumn_getPreviousMergeCell(cell, column->pColumn);
                if(mCell != NULL) { // Cell could be missing if previously pruned out
                    mCell->backwardLogProb = stMath_logAdd(cell->backwardLogProb, mCell->backwardLogProb);
                }
            }
            else {
                hmm->backwardLogProb = stMath_logAdd(hmm->backwardLogProb, backwardLogProb);
            }

            // Add to column backward probability
            column->backwardLogProb = stMath_logAdd(column->forwardLogProb, backwardLogProb);
        }
        while((cell = cell->nCell) != NULL);

        // Cleanup the bit count vectors
        free(bitCountVectors);

        if(column->pColumn == NULL) {
            break;
        }
        column = column->pColumn->pColumn;
    }
}

void stRPHmm_prune(stRPHmm *hmm, double posteriorProbabilityThreshold, int64_t minColumnDepthToFilter) {
    /*
     * Remove cells from hmm whos posterior probability is below the given threshold
     */

    // For each column
    stRPColumn *column = hmm->firstColumn;
    while(1) {

        // If column depth is greater than a threshold
        if(column->depth >= minColumnDepthToFilter) {
            // For each state
            stRPCell *cell = column->head;
            stRPCell **pCell = &(cell); // Pointer to previous cell, used to remove cells from the linked list
            do {
                // If the posterior probability is below the given threshold
                if(stRPCell_posteriorProb(cell, column) < posteriorProbabilityThreshold) {
                    // Remove the state from the linked list of states
                    *pCell = cell->nCell;

                    // Cleanup
                    stRPCell_destruct(cell);
                }
            } while((cell = cell->nCell) != NULL);
        }

        // Move on to the next merge column
        stRPMergeColumn *mColumn = column->nColumn;

        if(mColumn == NULL) {
            break;
        }

        // If the column depth of the merge column is greater than a threshold
        if(stRPMergeColumn_depth(mColumn) >= minColumnDepthToFilter) {
            //  For each merge state
            stList *mergeCells = stHash_getValues(mColumn->mergeCellsFrom);
            for(int64_t i=0; i<stList_length(mergeCells); i++) {
                stRPMergeCell *mCell = stList_get(mergeCells, i);

                // If the merge state has posterior probability below the given threshold
                if(stRPMergeCell_posteriorProb(mCell, mColumn) < posteriorProbabilityThreshold) {
                    // Remove the state from the merge column
                    stHash_remove(mColumn->mergeCellsFrom, &(mCell->fromPartition));
                    stHash_remove(mColumn->mergeCellsTo, &(mCell->toPartition));

                    // Cleanup
                    stRPMergeCell_destruct(mCell);
                }
            }
            stList_destruct(mergeCells);
        }

        column = mColumn->nColumn;
    }
}

bool stRPHmm_overlapOnReference(stRPHmm *hmm1, stRPHmm *hmm2) {
    /*
     * Return non-zero iff hmm1 and hmm2 have the same reference sequence and overlapping
     * coordinates intervals on that reference sequence.
     */

    // If either interval is zero length this is not a well defined comparison
    if(hmm1->refLength <= 0 || hmm2->refLength <= 0) {
        st_errAbort("Trying to compare HMMs with a zero length coordinate interval");
    }

    // Check if on the same reference sequence
    if(!stString_eq(hmm1->referenceName, hmm2->referenceName)) {
        return 0;
    }

    // Check if intervals overlap

    // If hmm1 starts after hmm2's start coordinate then switch hmm1 for hmm2
    if(hmm1->refStart > hmm2->refStart) {
        return stRPHmm_overlapOnReference(hmm2, hmm1);
    }

    // The coordinates of the first interval overlap the second
    return hmm1->refStart + hmm1->refLength > hmm2->refStart;
}

/*
 * Read partitioning hmm column (stRPColumn) functions
 */

stRPColumn *stRPColumn_construct(int64_t refStart, int64_t length, int64_t depth,
        stProfileSeq **seqHeaders, stProfileProb **seqs) {
    stRPColumn *column = st_malloc(sizeof(stRPColumn));

    // Reference coordinates
    column->refStart = refStart;
    column->length = length;

    // Sequences
    column->depth = depth;
    column->seqHeaders = seqHeaders;
    column->seqs = seqs;

    // Initially contains not states
    column->head = NULL;

    return column;
}

void stRPColumn_destruct(stRPColumn *column) {
    // Clean up the contained cells
    stRPCell *cell = column->head;
    while(cell != NULL) {
        stRPCell *pCell = cell;
        cell = cell->nCell;
        stRPCell_destruct(pCell);
    }

    free(column->seqHeaders);
    free(column->seqs);
    free(column);
}

void stRPColumn_print(stRPColumn *column, FILE *fileHandle, bool includeCells) {
    /*
     * Print a description of the column. If includeCells is true then print the
     * state of the cells too.
     */
    fprintf(fileHandle, "\tCOLUMN: REF_START: %" PRIi64
            " REF_LENGTH: %" PRIi64 " DEPTH: %" PRIi64
            " FORWARD_PROB: %f BACKWARD_PROB: %f\n",
            column->refStart, column->length, column->depth,
            (float)column->forwardLogProb, (float)column->backwardLogProb);
    for(int64_t i=0; i<column->depth; i++) {
        stProfileSeq_print(column->seqHeaders[i], fileHandle, 0);
    }
    if(includeCells) {
        stRPCell *cell = column->head;
        do {
            fprintf(fileHandle, "\t\t");
            stRPCell_print(cell, fileHandle);
        } while((cell = cell->nCell) != NULL);
    }
}

void stRPColumn_split(stRPColumn *column, int64_t firstHalfLength, stRPHmm *hmm) {
    /*
     * Split the column into two to divide into two smaller reference intervals
     */

    // Create column
    stProfileSeq **seqHeaders = st_malloc(sizeof(stProfileSeq *) * column->depth);
    memcpy(seqHeaders, column->seqHeaders, sizeof(stProfileSeq *) * column->depth);
    stProfileProb **seqs = st_malloc(sizeof(stProfileProb *) * column->depth);
    memcpy(seqs, column->seqs, sizeof(stProfileProb *) * column->depth);
    stRPColumn *rColumn = stRPColumn_construct(column->refStart+firstHalfLength,
            column->length-firstHalfLength, column->depth, seqHeaders, seqs);

    // Create merge column
    uint64_t acceptMask = makeAcceptMask(column->depth);
    stRPMergeColumn *mColumn = stRPMergeColumn_construct(acceptMask, acceptMask);

    // Copy cells
    stRPCell *cell = column->head;
    stRPCell **pCell = &(rColumn->head);
    do {
        *pCell = stRPCell_construct(cell->partition);
        stRPMergeCell_construct(cell->partition, cell->partition, mColumn);
        pCell = &((*pCell)->nCell);
    } while((cell = cell->nCell) != NULL);

    // Create links
    rColumn->pColumn = mColumn;
    mColumn->nColumn = rColumn;

    // If is the last column
    if(column->nColumn == NULL) {
       hmm->lastColumn = rColumn;
    }
    else {
        column->nColumn->pColumn = rColumn;
        rColumn->nColumn = column->nColumn;
    }
    column->nColumn = mColumn;
    mColumn->pColumn = column;

    // Increase column number
    hmm->columnNumber++;
}

/*
 * Read partitioning hmm state (stRPCell) functions
 */

stRPCell *stRPCell_construct(int64_t partition) {
    stRPCell *cell = st_calloc(1, sizeof(stRPCell));
    return cell;
}

void stRPCell_destruct(stRPCell *cell) {
    free(cell);
}

void stRPCell_print(stRPCell *cell, FILE *fileHandle) {
    /*
     * Prints a debug representation of the cell.
     */
    char *partitionString = intToBinaryString(cell->partition);
    fprintf(fileHandle, "CELL PARTITION: %s FORWARD_PROB: %f BACKWARD_PROB: %f\n",
            partitionString, (float)cell->forwardLogProb, (float)cell->backwardLogProb);
    free(partitionString);
}

double stRPCell_posteriorProb(stRPCell *cell, stRPColumn *column) {
    /*
     * Calculate the posterior probability of visiting the given cell. Requires that the
     * forward and backward algorithms have been run.
     */
    double p = exp(cell->forwardLogProb + cell->backwardLogProb - (column->forwardLogProb+column->backwardLogProb));
    assert(p <= 1.001);
    assert(p >= 0.0);
    return p > 1.0 ? 1.0 : p;
}

/*
 * Read partitioning hmm merge column (stRPMergeColumn) functions
 */

static uint64_t intHashFn(const void *a) {
    return *(uint64_t *)a;
}

static int intEqualsFn(const void *key1, const void *key2) {
    return *(uint64_t *)key1 == *(uint64_t *)key2;
}

stRPMergeColumn *stRPMergeColumn_construct(uint64_t maskFrom, uint64_t maskTo) {
    stRPMergeColumn *mColumn = st_calloc(1, sizeof(stRPMergeColumn));
    mColumn->maskFrom = maskFrom;
    mColumn->maskTo = maskTo;

    // Maps between partitions and cells
    mColumn->mergeCellsFrom = stHash_construct3(intHashFn, intEqualsFn, NULL, NULL);
    mColumn->mergeCellsTo = stHash_construct3(intHashFn, intEqualsFn, NULL, NULL);

    return mColumn;
}

void stRPMergeColumn_destruct(stRPMergeColumn *mColumn) {
    stHash_destruct(mColumn->mergeCellsFrom);
    stHash_destruct(mColumn->mergeCellsTo);
    free(mColumn);
}

void stRPMergeColumn_print(stRPMergeColumn *mColumn, FILE *fileHandle, bool includeCells) {
    /*
     * Print a debug representation of the merge column.
     */
    char *maskFromString = intToBinaryString(mColumn->maskFrom);
    char *maskToString = intToBinaryString(mColumn->maskTo);
    fprintf(fileHandle, "\tMERGE_COLUMN MASK_FROM: %s MASK_TO: %s"
            " DEPTH: %" PRIi64 "\n", maskFromString, maskToString,
            stHash_size(mColumn->mergeCellsFrom));
    assert(stHash_size(mColumn->mergeCellsFrom) == stHash_size(mColumn->mergeCellsTo));
    free(maskFromString);
    free(maskToString);
    if(includeCells) {
        stHashIterator *it = stHash_getIterator(mColumn->mergeCellsFrom);
        stRPMergeCell *mCell;
        while((mCell = stHash_getNext(it)) != NULL) {
            fprintf(fileHandle, "\t\t");
            stRPMergeCell_print(mCell, fileHandle);
        }
        stHash_destructIterator(it);
    }
}

stRPMergeCell *stRPMergeColumn_getNextMergeCell(stRPCell *cell, stRPMergeColumn *mergeColumn) {
    /*
     * Get the merge cell that this cell feeds into.
     */
    uint64_t i = maskPartition(cell->partition, mergeColumn->maskFrom);
    stRPMergeCell *mCell = stHash_search(mergeColumn->mergeCellsFrom, &i);
    return mCell;
}

stRPMergeCell *stRPMergeColumn_getPreviousMergeCell(stRPCell *cell, stRPMergeColumn *mergeColumn) {
    /*
     * Get the merge cell that this cell feeds from.
     */
    uint64_t i = maskPartition(cell->partition,  mergeColumn->maskTo);
    stRPMergeCell *mCell = stHash_search(mergeColumn->mergeCellsTo, &i);
    return mCell;
}

int64_t stRPMergeColumn_depth(stRPMergeColumn *mColumn) {
    /*
     * Returns the number of cells in the column.
     */
    return stHash_size(mColumn->mergeCellsFrom);
}

/*
 * Read partitioning hmm merge cell (stRPMergeCell) functions
 */

stRPMergeCell *stRPMergeCell_construct(uint64_t fromPartition, uint64_t toPartition,
        stRPMergeColumn *mColumn) {
    /*
     * Create a merge cell, adding it to the merge column mColumn.
     */
    stRPMergeCell *mCell = st_malloc(sizeof(stRPMergeCell));
    mCell->fromPartition = fromPartition;
    mCell->toPartition = toPartition;
    stHash_insert(mColumn->mergeCellsFrom, &mCell->fromPartition, mCell);
    stHash_insert(mColumn->mergeCellsTo, &mCell->toPartition, mCell);
    return mCell;
}

void stRPMergeCell_destruct(stRPMergeCell *mCell) {
    free(mCell);
}

void stRPMergeCell_print(stRPMergeCell *mCell, FILE *fileHandle) {
    /*
     * Prints a debug representation of the cell.
     */
    char *fromPartitionString = intToBinaryString(mCell->fromPartition);
    char *toPartitionString = intToBinaryString(mCell->toPartition);
    fprintf(fileHandle, "MERGE_CELL FROM_PARTITION: %s TO_PARTITION: %s "
            "FORWARD_PROB: %f BACKWARD_PROB: %f\n",
            fromPartitionString, toPartitionString,
            (float)mCell->forwardLogProb, (float)mCell->backwardLogProb);
    free(fromPartitionString);
    free(toPartitionString);
}

double stRPMergeCell_posteriorProb(stRPMergeCell *mCell, stRPMergeColumn *mColumn) {
    /*
     * Calculate the posterior probability of visiting the given cell. Requires that the
     * forward and backward algorithms have been run.
     */
    double p = exp(mCell->forwardLogProb + mCell->backwardLogProb -
            (mColumn->nColumn->forwardLogProb+mColumn->nColumn->backwardLogProb));
    assert(p <= 1.001);
    assert(p >= 0.0);
    return p > 1.0 ? 1.0 : p;
}
