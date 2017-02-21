#include <string.h>                         // strcmp()
#include <iostream>                         // atoi()
#include <thread>                           // std::thread and join()
#include <chrono>                           // std::chrono::high_resolution_clock::now();
#include <vector>                           // std::vector<>
#include <mutex>                            // std::mutex
#include <atomic>                           // std::atomic<bool>

/*
 * pause is an empty loop that iterates k times
 *
 * Input Arguments:
 * k - integer number of loop iterations
 *
 * Return Values:
 * None
 */
void pause(int k) {
    int iterator = 0;
    while(iterator < k) {
        ++iterator;
    }
}

class TicketLock {
public:
    
    std::atomic<int> next_ticket = ATOMIC_VAR_INIT(0);
    std::atomic<int> now_serving = ATOMIC_VAR_INIT(0);
    const double ticketLockBackoffBase = 1.25;

    /*
     * acquire gives the thread the the lock when
     * thread.my_ticket = thread.now_serving
     *
     * Input Arguments:
     * None
     *
     * Return Values:
     * None
     */
    void acquire() {
        int my_ticket = std::atomic_fetch_add(&next_ticket, 1);
        while(true) {
            int ns = now_serving.load(std::memory_order_relaxed);
            if(ns == my_ticket) {
                break;
            }
            atomic_thread_fence(std::memory_order_seq_cst);
        }
    }
    
    /*
     * acquire gives the thread the the lock when
     * thread.my_ticket = thread.now_serving using a backoff
     *
     * Input Arguments:
     * None
     *
     * Return Values:
     * None
     */
    void acquireBackoff() {
        int my_ticket = std::atomic_fetch_add(&next_ticket, 1);
        while(true) {
            int ns = now_serving.load(std::memory_order_relaxed);
            if(ns == my_ticket) {
                break;
            }
            pause(ticketLockBackoffBase * (my_ticket-ns));
            atomic_thread_fence(std::memory_order_seq_cst);
        }
    }

    /*
     * release increments now_serving
     *
     * Input Arguments:
     * None
     *
     * Return Values:
     * None
     */
    void release() {
        int t = now_serving + 1;
        now_serving.store(t,std::memory_order_relaxed);
    }

};

// qnodeMCS' are linked-list nodes that implement MCSLock
struct qnodeMCS {
    std::atomic<qnodeMCS*> next;
    std::atomic<bool> waiting;
};

class MCSLock {
public:
    
    std::atomic<qnodeMCS*> tail;
    
    /*
     * acquire acquires a MC5Lock
     *
     * Input Arguments:
     * p - qnodeMCS pointer allocated by thread
     *
     * Return Values:
     * None
     */
    void acquire(qnodeMCS* p) {
        p->next = NULL;
        p->waiting = true;
        qnodeMCS* prev;
        prev = tail.exchange(p, std::memory_order_seq_cst);
        if (prev != NULL) {
            prev->next.store(p, std::memory_order_acquire);
            while(p->waiting.load());
        }
        atomic_thread_fence(std::memory_order_acquire);
    }
    
    /*
     * release releases a MC5Lock
     *
     * Input Arguments:
     * p - qnodeMCS pointer allocated by thread
     *
     * Return Values:
     * None
     */
    void release(qnodeMCS* p) {
        qnodeMCS* succ = p->next.load();
        if (succ == nullptr) {
            if(tail.compare_exchange_strong(p, NULL, std::memory_order_acquire, std::memory_order_seq_cst)) {
                return;
            }
            while(succ != nullptr) {
                succ = p->next.load();
            }
        }
        succ->waiting.store(false);
    }
    
};

// qnodeK42MCS' are nodes that implement MCSLock
struct qnodeK42MCS {
    std::atomic<qnodeK42MCS*> tail;
    std::atomic<qnodeK42MCS*> next;
    
    qnodeK42MCS(qnodeK42MCS* tail, qnodeK42MCS* next) : tail(tail), next(next) {}
};

const qnodeK42MCS* WAITING = (qnodeK42MCS*) 1;

class K42MCSLock {
public:
    
    qnodeK42MCS q;
    K42MCSLock() : q(NULL, NULL) {}
    
    /*
     * acquire acquires a K42MCSLock
     *
     * Input Arguments:
     * None
     *
     * Return Values:
     * None
     */
    void acquire() {
        while(true) {
            qnodeK42MCS* prev = q.tail.load();
            if (prev == NULL) {                                                                     // lock appears to be free
                qnodeK42MCS* nullPtr = nullptr;
                if(q.tail.compare_exchange_strong(nullPtr, &q, std::memory_order_seq_cst)) {
                    break;
                }
            }
            else {
                qnodeK42MCS n((qnodeK42MCS*)WAITING, NULL);
                if(q.tail.compare_exchange_strong(prev, &n, std::memory_order_seq_cst)) {           // we're in line
                    prev->next.store(&n);
                    while(n.tail.load() == WAITING);                                                // spin
                    // now we have the lock
                    qnodeK42MCS* succ = n.next.load();
                    if(succ == NULL) {
                        q.next.store(NULL);
                        // try to make lock point at itself
                        qnodeK42MCS* nAddress = &n;
                        if(!q.tail.compare_exchange_strong(nAddress, &q, std::memory_order_seq_cst)) {
                            // somebody got into the timing window
                            while(succ == NULL) {
                                succ = n.next.load();
                            }
                            q.next.store(succ);
                        }
                        break;
                    }
                    else {
                        q.next.store(succ);
                        break;
                    }
                }
            }
        }
        atomic_thread_fence(std::memory_order_seq_cst);
    }

    /*
     * release releases a K42MCSLock
     *
     * Input Arguments:
     * None
     *
     * Return Values:
     * None
     */
    void release() {
        qnodeK42MCS* succ = q.next.load(std::memory_order_seq_cst);
        if (succ == NULL) {
            qnodeK42MCS* qAddress = &q;
            if(q.tail.compare_exchange_strong(qAddress, NULL, std::memory_order_seq_cst)) {
                return;
            }
            while(succ == NULL) {
                succ = q.next.load();
            }
        }
        succ->tail.store(NULL);
    }
    
};

std::mutex sharedCounter_mtx;
std::atomic<bool> start(false);             // to ensure threads run in parallel
std::atomic_flag lock = ATOMIC_FLAG_INIT;
TicketLock ticketLock;
MCSLock mcsLock;
K42MCSLock k42MCSLock;

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
        pause(tasBackoffValue);
        tasBackoffValue = std::min(tasBackoffValue*tasBackoffMultiplier, tasBackoffCap);
    }
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    lock.clear();
}

/*
 * incrementiTimesTicketBackoff will acquire a TicketLock, run the commnad
 * '++sharedCounter' i times, then release the TicketLock
 *
 * * Input Arguments:
 * sharedCounter - reference to variable increment i times
 * i - reference to the number of times to increment sharedCounter
 *
 * Return Values:
 * None
 */
void incrementiTimesTicket(int& sharedCounter, int& i) {
    ticketLock.acquire();
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    ticketLock.release();
}

/*
 * incrementiTimesTicketBackoff will acquire a TicketLock with a backoff,
 * run the commnad '++sharedCounter' i times, then release the TicketLock
 *
 * * Input Arguments:
 * sharedCounter - reference to variable increment i times
 * i - reference to the number of times to increment sharedCounter
 *
 * Return Values:
 * None
 */
void incrementiTimesTicketBackoff(int& sharedCounter, int& i) {
    ticketLock.acquireBackoff();
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    ticketLock.release();
}

/*
 * incrementiTimesMCS will acquire a MCSLock,
 * run the commnad '++sharedCounter' i times, then release the MCSLock
 *
 * * Input Arguments:
 * sharedCounter - reference to variable increment i times
 * i - reference to the number of times to increment sharedCounter
 *
 * Return Values:
 * None
 */
void incrementiTimesMCS(int& sharedCounter, int& i) {
    std::atomic<qnodeMCS*> p;
    p = new (qnodeMCS);
    mcsLock.acquire(p);
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    mcsLock.release(p);
}

/*
 * incrementiTimesMCS will acquire a K42MCSLock,
 * run the commnad '++sharedCounter' i times, then release the K42MCSLock
 *
 * * Input Arguments:
 * sharedCounter - reference to variable increment i times
 * i - reference to the number of times to increment sharedCounter
 *
 * Return Values:
 * None
 */
void incrementiTimesK42MCS(int& sharedCounter, int& i) {
    k42MCSLock.acquire();
    for(int incrementCounter = 0; incrementCounter < i; ++incrementCounter) {
        ++sharedCounter;
    }
    k42MCSLock.release();
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

    /*
     * t threads each increment sharedCounter in parallel i times using a ticket lock
     * sharedCounter will be set to i*t
     *
     * t, Increments/Millisecond, and seconds will be printed, threadVector, sharedCounter
     * and start will be reset
     */
    for(int iterator = 0; iterator < t; ++iterator) {
        threadVector.push_back(std::thread(incrementiTimesTicket, std::ref(sharedCounter), std::ref(i)));
    }
    t1 = std::chrono::high_resolution_clock::now();
    start = true;
    for(auto& t : threadVector) {
        t.join();
    }
    t2 = std::chrono::high_resolution_clock::now();
    tDelta = t2-t1;
    seconds = tDelta.count();
    std::cout << "incrementiTimesTicket\t" << sharedCounter<< "\t" << t << "\t" << sharedCounter*1000/seconds << "\t" << seconds << "\n";
    threadVector.clear();
    sharedCounter = 0;
    start = false;
    
    /*
     * t threads each increment sharedCounter in parallel i times using a ticket lock with a proportional backoff
     * sharedCounter will be set to i*t
     *
     * t, Increments/Millisecond, and seconds will be printed, threadVector, sharedCounter
     * and start will be reset
     */
    for(int iterator = 0; iterator < t; ++iterator) {
        threadVector.push_back(std::thread(incrementiTimesTicketBackoff, std::ref(sharedCounter), std::ref(i)));
    }
    t1 = std::chrono::high_resolution_clock::now();
    start = true;
    for(auto& t : threadVector) {
        t.join();
    }
    t2 = std::chrono::high_resolution_clock::now();
    tDelta = t2-t1;
    seconds = tDelta.count();
    std::cout << "incrementiTimesTicketBackoff\t" << sharedCounter<< "\t" << t << "\t" << sharedCounter*1000/seconds << "\t" << seconds << "\n";
    threadVector.clear();
    sharedCounter = 0;
    start = false;
    
    /*
     * t threads each increment sharedCounter in parallel i times using a MCS lock
     * sharedCounter will be set to i*t
     *
     * t, Increments/Millisecond, and seconds will be printed, threadVector, sharedCounter
     * and start will be reset
     */
    for(int iterator = 0; iterator < t; ++iterator) {
        threadVector.push_back(std::thread(incrementiTimesMCS, std::ref(sharedCounter), std::ref(i)));
    }
    t1 = std::chrono::high_resolution_clock::now();
    start = true;
    for(auto& t : threadVector) {
        t.join();
    }
    t2 = std::chrono::high_resolution_clock::now();
    tDelta = t2-t1;
    seconds = tDelta.count();
    std::cout << "incrementiTimesMCS\t" << sharedCounter<< "\t" << t << "\t" << sharedCounter*1000/seconds << "\t" << seconds << "\n";
    threadVector.clear();
    sharedCounter = 0;
    start = false;
    
    /*
     * t threads each increment sharedCounter in parallel i times using a K42 MCS lock
     * sharedCounter will be set to i*t
     *
     * t, Increments/Millisecond, and seconds will be printed, threadVector, sharedCounter
     * and start will be reset
     */
    for(int iterator = 0; iterator < t; ++iterator) {
        threadVector.push_back(std::thread(incrementiTimesK42MCS, std::ref(sharedCounter), std::ref(i)));
    }
    t1 = std::chrono::high_resolution_clock::now();
    start = true;
    for(auto& t : threadVector) {
        t.join();
    }
    t2 = std::chrono::high_resolution_clock::now();
    tDelta = t2-t1;
    seconds = tDelta.count();
    std::cout << "incrementiTimesK42MCS\t" << sharedCounter<< "\t" << t << "\t" << sharedCounter*1000/seconds << "\t" << seconds << "\n";
    threadVector.clear();
    sharedCounter = 0;
    start = false;

}