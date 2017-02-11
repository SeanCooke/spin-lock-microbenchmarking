#include <string.h>                         // strcmp()
#include <iostream>                         // atoi()
#include <thread>                           // std::thread and join()
#include <chrono>                           // std::chrono::high_resolution_clock::now();
#include <vector>                           // std::vector<>
#include <mutex>                            // std::mutex
#include <atomic>                           // std::atomic<bool>
#include <unistd.h>                         // usleep()

std::mutex sharedCounter_mtx;
std::atomic<bool> start(false);             // to ensure threads run in parallel
std::atomic_flag lock = ATOMIC_FLAG_INIT;

/*
 * incrementiTimesMutexLock will lock a mutex, run the commnad '++sharedCounter'
 * i times, then unlock the mutex once start is set to true
 *
 * Input Arguments:
 * sharedCounter - reference to variable to lock, increment i times, and unlock
 * i - reference to the number of times to increment sharedCounter
 *
 * Return Values:
 * None
 */
void incrementiTimesMutexLock(int& sharedCounter, int& i) {
    while (!start.load());
    sharedCounter_mtx.lock();
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    sharedCounter_mtx.unlock();
}

/*
 * incrementiTimesNaiveTAS will spin until lock is clear, lock the lock,
 * run the commnad '++sharedCounter' i times, then clear the lock
 *
 * Input Arguments:
 * sharedCounter - reference to variable increment i times
 * i - reference to the number of times to increment sharedCounter
 *
 * Return Values:
 * None
 */
void incrementiTimesNaiveTAS(int& sharedCounter, int& i) {
    while (!start.load());
    while (lock.test_and_set());
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    lock.clear();
}

/*
 * incrementiTimesTASBackoff will issue a TAS at an exponentially increasing
 * number of microseconds, starting at tasBackoffBase, increasing by a factor
 * of tasBackoffMultiplier before reaching tasBackoffCap.
 * Once the lock has been acquired, the command '++sharedCounter' is run i times
 * and the lock is cleared
 *
 * Input Arguments:
 * sharedCounter - reference to variable increment i times
 * i - reference to the number of times to increment sharedCounter
 * tasBackoffBase - initial number of microseconds to sleep between
 * TAS loop iterations
 * tasBackoffCap - maximum number of microseconds to sleep between
 * TAS loop iterations
 * tasBackoffMultiplier - factor to multiply number of microseconds
 * to sleep between TAS loop iterations
 *
 * Return Values:
 * None
 */
void incrementiTimesTASBackoff(int& sharedCounter, int& i, int tasBackoffBase, double tasBackoffCap, double tasBackoffMultiplier) {
    while (!start.load());
    double tasBackoffValue = tasBackoffBase;
    while (lock.test_and_set()) {
        usleep(tasBackoffValue);
        tasBackoffValue = std::min(tasBackoffValue*tasBackoffMultiplier, tasBackoffCap);
    }
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    lock.clear();
}

int main(int argc, char *argv[]) {

    // Default values for t and i are 4 and 10000, respectively
    int t = 4;
    int i = 10000;
    int sharedCounter = 0;
    int tasBackoffBase = 1;
    double tasBackoffCap = 4;
    double tasBackoffMultiplier = 1.25;
    std::vector<std::thread> threadVector;

    /*
     * Parsing command line arguments...
     * Argument directly following "-t" (if any) will be t
     * Argument directly following "-i" (if any) will be i
     * If multiple "-t" or multiple "-i" flags are found, the last one will be used
     * If "-t" or "-i" is the last command line argument, it will be ignored
     */
    int lastIndexToCheck = argc-1;
    for(int argcIterator = 1; argcIterator < lastIndexToCheck; ++argcIterator) {
        if(strcmp(argv[argcIterator], "-t") == 0) {
            argcIterator += 1;
            t = atoi(argv[argcIterator]);
        }
        else if (strcmp(argv[argcIterator], "-i") == 0) {
            argcIterator += 1;
            i = atoi(argv[argcIterator]);
        }
    }
    
    // std::cout << "Function Name\tFinal Counter Value\tThreads\tIncrements/Millisecond\tSeconds\n";
    
    /*
     * t threads each increment sharedCounter in parallel i times using a mutex
     * sharedCounter will be set to i*t
     *
     * t, Increments/Millisecond, and seconds will be printed, threadVector, sharedCounter
     * and start will be reset
     */
    for(int iterator = 0; iterator < t; ++iterator) {
        threadVector.push_back(std::thread(incrementiTimesMutexLock, std::ref(sharedCounter), std::ref(i)));
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    start = true;
    for(auto& t : threadVector) {
        t.join();
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> tDelta = t2-t1;
    auto seconds = tDelta.count();
    std::cout << "incrementiTimesMutexLock\t" << sharedCounter<< "\t" << t << "\t" << sharedCounter*1000/seconds << "\t" << seconds << "\n";
    threadVector.clear();
    sharedCounter = 0;
    start = false;

    /*
     * t threads each increment sharedCounter in parallel i times using a Naive Test And Set (TAS) lock
     * sharedCounter will be set to i*t
     *
     * t, Increments/Millisecond, and seconds will be printed, threadVector, sharedCounter
     * and start will be reset
     */
    for(int iterator = 0; iterator < t; ++iterator) {
        threadVector.push_back(std::thread(incrementiTimesNaiveTAS, std::ref(sharedCounter), std::ref(i)));
    }
    t1 = std::chrono::high_resolution_clock::now();
    start = true;
    for(auto& t : threadVector) {
        t.join();
    }
    t2 = std::chrono::high_resolution_clock::now();
    tDelta = t2-t1;
    seconds = tDelta.count();
    std::cout << "incrementiTimesNaiveTAS\t" << sharedCounter<< "\t" << t << "\t" << sharedCounter*1000/seconds << "\t" << seconds << "\n";
    threadVector.clear();
    sharedCounter = 0;
    start = false;
    
    /*
     * t threads each increment sharedCounter in parallel i times using a TAS lock with an exponential backoff and maximum
     * sharedCounter will be set to i*t
     *
     * t, Increments/Millisecond, and seconds will be printed, threadVector, sharedCounter
     * and start will be reset
     */
    for(int iterator = 0; iterator < t; ++iterator) {
        threadVector.push_back(std::thread(incrementiTimesTASBackoff, std::ref(sharedCounter), std::ref(i), tasBackoffBase, tasBackoffCap, tasBackoffMultiplier));
    }
    t1 = std::chrono::high_resolution_clock::now();
    start = true;
    for(auto& t : threadVector) {
        t.join();
    }
    t2 = std::chrono::high_resolution_clock::now();
    tDelta = t2-t1;
    seconds = tDelta.count();
    std::cout << "incrementiTimesTASBackoff\t" << sharedCounter<< "\t" << t << "\t" << sharedCounter*1000/seconds << "\t" << seconds << "\n";
    threadVector.clear();
    sharedCounter = 0;
    start = false;
    
}
