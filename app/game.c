#include <stdint.h>

static uint32_t randSeed = 1;

// Initialise seed
void srand_custom(uint32_t seed) {
    randSeed = seed;
}

// Return pseudo-random from 0 to RAND_MAX (here 32767)
int rand_custom(void) {
    randSeed = randSeed * 1103515245 + 12345;
    return (randSeed >> 16) & 0x7FFF; // 15 bits
}

// Return integer from min to max include
int randInt(int min, int max) {
    return min + (rand_custom() % (max - min + 1));
}