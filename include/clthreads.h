// ---------------------------------------------------------------------------------
//
//  Copyright (C) 2003-2008 Fons Adriaensen <fons@kokkinizita.net>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU Lesser General Public License as published
//  by the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this program; if not, write to the Free Software
//  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// Thomas and Mathis Braschler, 2025: Completion with the libbthread library for thread
// cancelling. Also, some more comments, in part based on claude.ai
// ---------------------------------------------------------------------------------


#ifndef CLTHREADS_H
#define CLTHREADS_H


#include <sys/types.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cassert>
#include <cerrno>
#include <pthread.h>
#include "../libbthread/bthread.h"

#include <semaphore.h>
/** POSIX semaphore wrapper for signaling between threads
 *
 */
class P_sema
{
public:

    P_sema () { if (sem_init (&_sema, 0, 0)) abort (); }
    ~P_sema () { sem_destroy (&_sema); }
    P_sema (const P_sema&);
    P_sema& operator= (const P_sema&);

    void post () { if (sem_post (&_sema)) abort (); }
    void wait () { if (sem_wait (&_sema)) abort (); }
    int trywait  () { return sem_trywait (&_sema); }
    int getvalue () { int n; sem_getvalue (&_sema, &n); return n; }

private:

    sem_t  _sema{};
};

/**
 * Wrapper around p-thread mutex
 */
class Bmutex
{
public:

    Bmutex () { if (pthread_mutex_init (&_mutex, nullptr)) abort (); }
    ~Bmutex () { pthread_mutex_destroy (&_mutex); }
    Bmutex (const Bmutex&);
    Bmutex& operator= (const Bmutex&);

    void lock () { if (pthread_mutex_lock (&_mutex)) abort (); }
    void unlock (){ if (pthread_mutex_unlock (&_mutex)) abort (); }
    int trylock () { return pthread_mutex_trylock (&_mutex); }

private:

    friend class Esync;

    pthread_mutex_t  _mutex{};
};


// -------------------------------------------------------------------------------------------

/**
 * Cmutex - A recursive mutex wrapper class
 *
 * This class wraps pthread_mutex_t to provide recursive locking capabilities.
 * A recursive mutex allows the same thread to lock the mutex multiple times
 * without causing a deadlock, as long as it unlocks the same number of times.
 */
class Cmutex
{
public:

    Cmutex () : _owner (0), _count (0) { if (pthread_mutex_init (&_mutex, 0)) abort (); }
    ~Cmutex () { pthread_mutex_destroy (&_mutex); }
    // Copy constructor and assignment operator are declared but not defined
    // This prevents copying of mutex objects (mutexes should not be copied)
    Cmutex (const Cmutex&);

    Cmutex& operator= (const Cmutex&);

    void lock ();
    void unlock ();

private:

    pthread_mutex_t   _mutex{};
    pthread_t         _owner; // Thread ID of the current owner (0 if unlocked)
    int               _count; // Number of times the current owner has locked this mutex
};


inline void Cmutex::lock ()
{
    if (_owner == pthread_self ()) ++_count;
    else
    {
        pthread_mutex_lock (&_mutex);
        _owner = pthread_self ();
        _count = 1;
    }
}

inline void Cmutex::unlock ()
{
    if (_owner == pthread_self ())
    {
        if (--_count == 0)
        {
            _owner = 0;
            pthread_mutex_unlock (&_mutex);
        }
    }
}


// -------------------------------------------------------------------------------------------

/** Event synchronization with condition variables and bitmasks */
class Esync : public Bmutex
{
public:

    enum
    {
        EM_ALL   = ~0,
        EV_TIME  = -1,
        EV_ERROR = -2
    };

    Esync () : _event (EV_ERROR), _emask (0) { if (pthread_cond_init (&_cond, 0)) abort (); }
    ~Esync () { pthread_cond_destroy (&_cond); }
    Esync (const Esync&);
    Esync& operator= (const Esync&);

    /**
     * Register the id of the last even
     * @param e Event id
     */
    void eput (int e);
    /**
     * Get the id of the last event matching the mask
     * @param m Event mask
     * @param t If desired, allocated timespec structure to return the result
     * @return event ID, EVENT_ERROR if not found or other error
     */
    int  eget (unsigned int m = EM_ALL, const timespec *t = nullptr);

private:

    volatile int     _event; // last event
    unsigned int     _emask; // mask
    pthread_cond_t   _cond{};
};


inline void Esync::eput (int e)
{
    if ((1 << e) & _emask)
    {
        _event = e;
        if (pthread_cond_signal (&_cond)) abort ();
    }
}


inline int Esync::eget (unsigned int m, const timespec *t)
{
    int r;

    _event = EV_ERROR;
    _emask = m;

    do
    {
        if (t) r = pthread_cond_timedwait (&_cond, &_mutex, t);
        else   r = pthread_cond_wait (&_cond, &_mutex);
        if (_event >= 0) break;
        if (r == ETIMEDOUT)
        {
            _event = EV_TIME;
            break;
        }
    }
    while (r == EINTR);

    _emask = 0;
    return _event;
}


// -------------------------------------------------------------------------------------------

/** Base class for exchanging messages between threads */
class ITC_mesg
{
public:

    enum
    {
        ITM_CLLIB_BASE = 0x80000000,
    };

    explicit ITC_mesg (unsigned long type = 0) : _forw (nullptr), _back (nullptr), _type (type) { _counter++; }
    virtual ~ITC_mesg () { _counter--; }

    /** Destroy this message, freeing memory */
    virtual void recover () { delete this; }
    /** Pointer to the next message in the message list */
    ITC_mesg *forw (void) const { return _forw; }
     /** Pointer to the previous message in the message list */
    ITC_mesg *back (void) const { return _back; }
    /** Numerical code for message type */
    [[nodiscard]] unsigned long  type () const { return _type; }

    /** How many messages have been created already */
    static size_t object_counter () { return _counter; }

private:

    friend class ITC_list;

    ITC_mesg      *_forw; // pointer to next message
    ITC_mesg      *_back; // pointer to previous message
    unsigned long  _type; // message type, numerical value to be defined by subclasses

    static size_t _counter; // common message counter
};


// -------------------------------------------------------------------------------------------

/**
 * @brief ITC_list - Inter-Thread Communication message list
 *
 * This class implements a doubly-linked, first-in first-out list for managing ITC_mesg objects.
 * Designed for thread-safe message passing between threads,
 * maintaining pointers to head and tail for efficient insertion/removal.
 */

class ITC_list
{
public:

    ITC_list () : _head (nullptr), _tail (nullptr), _count (0) {}
    ~ITC_list () { flush (); }
    // Copy constructor and assignment operator are declared but not defined
    // This prevents copying of the message list (for thread safety)
    ITC_list (const ITC_list&);
    ITC_list& operator= (const ITC_list&);

    // Add message to tail of list
    void put (ITC_mesg *p);
    // Remove and return message from head of list
    ITC_mesg *get ();
    // Remove specific message from anywhere in list
    void rem (ITC_mesg *p);
    // Remove all messages and call recover() on each
    void flush (void);
    ITC_mesg *head (void) const { return _head; }
    ITC_mesg *tail (void) const { return _tail; }

    int count (void) const { return _count; }

private:

    ITC_mesg   *_head;
    ITC_mesg   *_tail;
    int         _count;
};


inline void ITC_list::put (ITC_mesg *p)
{
    if (p)
    {
        p->_forw = nullptr;
        p->_back = _tail;
        if (_tail) _tail->_forw = p;
        else              _head = p;
        _tail = p;
        ++_count;
    }
}


inline ITC_mesg *ITC_list::get ()
{
    ITC_mesg *p = _head;

    if (p)
    {
        _head = p->_forw;
        if (_head) _head->_back = nullptr;
        else              _tail = nullptr;
        p->_forw = p->_back = nullptr;
        --_count;
    }
    return p;
}


inline void ITC_list::rem (ITC_mesg *p)
{
    if (p == _head) _head = p->_forw;
    else  p->_back->_forw = p->_forw;
    if (p == _tail) _tail = p->_back;
    else  p->_forw->_back = p->_back;
    p->_forw = p->_back = nullptr;
    --_count;
}


inline void ITC_list::flush ()
{
    ITC_mesg *p = _head;
    while (p)
    {
        _head = p->_forw;
        p->recover ();
        p = _head;
    }
    _tail  = nullptr;
    _count = 0;
}


// -------------------------------------------------------------------------------------------

/**
 * @brief Event destination
 *
 * Virtual base class for objects capable to receive events through the clthreads framework
 */

class Edest
{
public:

    enum
    {
        NO_ERROR  = 0,
        NOT_CONN  = 1,
        DST_LOCK  = 2,
        BAD_PORT  = 3
    };

    Edest () {}
    virtual ~Edest () {}
    Edest (const Edest&);
    Edest& operator= (const Edest&);

    /**
     * Put event message into incoming event queue
     * @param evid Event ID
     * @param M ITC-type message
     * @return 0 on success, error code otherwise
     */
    virtual int  put_event (unsigned int evid, ITC_mesg *M) = 0;
    /**
     * Put basic event into incoming basic event queue
     * @param evid Event ID
     * @param M increment event count for this type of event by M
     * @return 0 on success, error code otherwise
     */
    virtual int  put_event (unsigned int evid, unsigned int incr) = 0;
    /**
     * Put basic event into incoming basic event queue, using trylock for the thread locking
     * @param evid Event ID
     * @param M increment event count for this type of event by M
     * @return 0 on success, error code otherwise
     */
    virtual int  put_event_try (unsigned int evid, unsigned int incr) = 0;
    /**
     * Remove the events of type evid in the event queue
     * @param evid Event ID
     */
    virtual void ipflush (unsigned evid) = 0;

private:
};


// -------------------------------------------------------------------------------------------

/**
 * Event receiver with a single input queue
 */
class ITC_ip1q : public Edest, protected Esync
{
public:

    enum {  N_BE = 31,  N_MQ =  1,  EM_ALL = ~0 };

    ITC_ip1q () : _bits (0), _mptr (nullptr) {};
    ~ITC_ip1q () {};
    ITC_ip1q (const ITC_ip1q&);
    ITC_ip1q& operator= (const ITC_ip1q&);

    /**
     * @brief Notify object of incoming basic event.
     *
     * For the basic events, there is no queue in ITC_ip1q. The pending events are instead stored via
     * a bitmask (_bits) instead
     * @param evid Event ID
     * @param incr Increment, here, for the event to be received, needs to be at least 1
     * @return 0 on success, error code otherwise
     */
    int put_event (unsigned int evid, unsigned int incr) override;
    /**
   * Notify object of incoming ITC message event. There is a single queue in ITC_ip1q, into which the
   * message is stored regardless of event id. The event id is used to update the bitmask (_bits)
   * @param evid Event ID
   * @param M ITC message
   * @return 0 on success, error code otherwise
   */
    int put_event (unsigned int evid, ITC_mesg *M) override;
    /**
     * Notify object of incoming basic event, abandoning if the thread lock
     * cannot be obtained. For the basic events, there is no queue in ITC_ip1q,
     * instead, the last basic event is stored in the _bits field
     * @param evid Event ID
     * @param incr Increment, here, for the event to be received, needs to be at least 1
     * @return 0 on success, error code otherwise
     */
    int put_event_try (unsigned int evid, unsigned int incr) override;
    /**
     * Remove event to be treated if it matches evid
     * @param evid Binary mask for removing the event
     */
    void  ipflush (unsigned int evid) override;
    /**
     * Get event provided it matches the mask
     * @param emask Event mask
     * @return 0 if ITC_msg type of event is queued for treatment, -1 on error, and higher numbers
     *         for basic events
     */
    int   get_event (unsigned int emask = EM_ALL);
    /**
     * Get event provided it matches the mask. Try to get the event immediately (no waiting on incoming events)
     * @param emask Event mask
     * @return 0 if ITC_msg type of event is queued for treatment, -1 on error, and higher numbers
     *         for basic events
     */
    [[maybe_unused]] int   get_event_nowait (unsigned int emask = EM_ALL);

    /**
     * Get the last identified ITC message
     * @return Pointer to the last ITC message
     */
    [[nodiscard]] ITC_mesg *get_message () const { return _mptr; }

private:
    /**
     * Find the highest priority event complying with the binary mask
     * @param mask Binary mask for event search
     * @return top priority event (highest bits present while matching mask)
     */
    int find_event (unsigned int mask) const;

    unsigned int  _bits;
    ITC_list      _list;
    ITC_mesg     *_mptr;
};


inline int ITC_ip1q::put_event (unsigned int evid, unsigned int incr)
{
    int r = NO_ERROR;
    assert (incr);
    lock ();
    if ((evid >= N_MQ) && (evid < N_MQ + N_BE))
    {
        _bits |= 1 << evid;
        eput (evid);
    }
    else  r = BAD_PORT;
    unlock ();
    return r;
}


inline int ITC_ip1q::put_event (unsigned int evid, ITC_mesg *M)
{
    int r = NO_ERROR;
    assert (M);
    lock ();
    if (evid < N_MQ)
    {
        _list.put (M);
        eput (evid);
    }
    else r = BAD_PORT;
    unlock ();
    return r;
}


inline int ITC_ip1q::put_event_try (unsigned int evid, unsigned int incr)
{
    int r = NO_ERROR;
    assert (incr);
    if (trylock ()) return DST_LOCK;
    if ((evid >= N_MQ) && (evid < N_MQ + N_BE))
    {
        _bits |= 1 << evid;
        eput (evid);
    }
    else  r = BAD_PORT;
    unlock ();
    return r;
}

inline void ITC_ip1q::ipflush (unsigned int evid)
{
    lock ();
    if (evid) _bits &= ~(1 << evid);
    else      _list.flush ();
    unlock ();
}


inline int ITC_ip1q::find_event (unsigned int mask) const
{
    int          i;
    unsigned int b;

    for (b = mask & _bits, i = 31; b; b <<= 1, i--)
    {
        if (b & 0x80000000) return i;
    }
    if ((mask & 1) && _list.head ()) return 0;

    return EV_TIME;
}


// -------------------------------------------------------------------------------------------

/**
 * Inter-thread communication ITC controller
 */
class ITC_ctrl : public Edest, protected Esync
{
public:

    enum
    {
        N_EC = 16,
        N_MQ = 16,
        EM_EC = (int) 0xFFFF0000,
        EM_MQ = (int) 0x0000FFFF,
        EM_ALL = EM_EC | EM_MQ,
        N_OP = 32
    };

    ITC_ctrl ();
    ~ITC_ctrl ();
    ITC_ctrl (const ITC_ctrl&);
    ITC_ctrl& operator= (const ITC_ctrl&);

    /**
     * @brief Send event using preconfigured output id opid
     *
     * Before using send_event, you need to use connect to configure the
     * destination for the opid and this as the source. Once this
     * configuration done, send_event can be used to dispatch to the preconfigured destination.
     * @param opid Output ID
     * @param M Message to be send
     * @return 0 if succesful, error code otherwise
     */
    int send_event (unsigned int opid, ITC_mesg *M);
    /**
     * @brief Send elementary thread synchronization event
     *
     * This function can be used to send basic events, like asking a thread to quit or timing events
     * without the need for genering an ITC_mesg.
     * @param opid Event ID
     * @param incr Increment the number of these events
     * @return 0 if successful, error code otherwise
     */
    int send_event (unsigned int opid, unsigned int incr);
     /**
      * Get the next event in line that matches the mask.
      * @param emask The event mask for binary mask comparison
      * @return Event id. If between 0 and N_MQ-1 (here between 0 and 15), this is an event with a message and
      *                   as a side effect, the _mptr field will point to the event.
      *                   Higher event ID's are for elementary events without associated ITC_msg. Negative
      *                   numbers indicate that no event corresponding to the mask was found.
      */
    int get_event (unsigned int emask = EM_ALL);
    /**
     * Get the next event in line that matches the mask, as well as time of reception in the filed _time
     * @param emask The event mask for binary mask comparison
     * @return Event id. If between 0 and N_MQ-1 (here between 0 and 15), this is an event with a message and
     *                   as a side effect, the _mptr field will point to the event.
     *                   Higher event ID's are for elementary events without associated ITC_msg. Negative
     *                   numbers indicate that no event corresponding to the mask was found.
     */
    int get_event_timed (unsigned int emask = EM_ALL);
    /** Try to get event immediately. If the thread lock cannot be obtained immediatly, return EV_TIME event
     * @param emask Mask that event needs to match
     * @return Event ID
     */
    int get_event_nowait (unsigned int emask = EM_ALL);
    /**
     * Set time stamp
     * @param t The time stamp to set
     */
    void set_time (const timespec *t = nullptr);
    /**
     * Read the present time stamp
     * @param t Pointer to timespec structure that will be filled to return the present time stamp
     */
    void get_time (timespec *t) { t->tv_sec = _time.tv_sec; t->tv_nsec = _time.tv_nsec; }
    /**
     * Increment present time stamp
     * @param micros Microseconds to add to time stamp
     */
    void inc_time (unsigned long micros);
    /**
     * How long ago is the present time stamp compared to processor time?
     * @return In microseconds, how far is the present processor time ahead of the set time stamp
     */
    unsigned long delay ();

    int  put_event (unsigned int evid, ITC_mesg *M) override;
    int  put_event (unsigned int evid, unsigned int incr) override;
    int  put_event_try (unsigned int evid, unsigned int incr) override;
    void ipflush (unsigned int evid) override;



    [[nodiscard]] ITC_mesg *get_message () const { return _mptr; }

    /**
     * @brief Connect thread objects for messaging between the threads.
     *
     * For a given source and output ID (opid), only one destination and input notification ID (ipid) can be configured;
     * a new call to connect for the same source and opid
     * will overwrite destination and ipid for that particular source and opid.<br />
     * In the sending process, the source will call its own send_event function, and indicate the opid along with the message. This
     * will cause the message to be sent to the destination object (via call to the destination object's put event)
     * configured here, along with notification with ipid.
     * @param srce Thread that will emit the messages
     * @param opid output id (arbitrary, but re-use for message sending)
     * @param dest destination thread
     * @param ipid input id for the destination thread
     */
    static void connect (ITC_ctrl *srce, unsigned int opid,
                         Edest    *dest, unsigned int ipid);

private:
    /**
     * Find first event matching the binary mask (bitwise, 1 bit match sufficient)
     * @param emask
     * @return event id. If the event is an ITC_msg type event, id from 0 to N_MQ (0 to 15),
     *                   if the event is a basic event without ITC_msg, from N_MQ and upwards
     *                   Negative (e.g. EV_TIME=-1) if no event was found
     */
    [[nodiscard]] int find_event (unsigned int emask) const;

    ITC_list        _list [N_MQ]; // input message queues for different input ids
    unsigned int    _ecnt [N_EC]; // counter for simple (non-ITC_msg) type events
    ITC_mesg       *_mptr; // Pointer to last message found
    timespec        _time; // Timeout to wait for messages via get_event_timed
    Edest          *_dest [N_OP]; // destinations for sending messages
    int             _ipid [N_OP]; // input ids to be transmitted to the destination along with the message

};


inline int ITC_ctrl::put_event_try (unsigned int evid, unsigned int incr)
{
    int r = NO_ERROR;
    assert (incr);
    if (trylock ()) return DST_LOCK;
    if ((evid >= N_MQ) && (evid < N_MQ + N_EC))
    {
        _ecnt [evid - N_MQ] += incr;
        eput (evid);
    }
    else  r = BAD_PORT;
    unlock ();
    return r;
}


inline int ITC_ctrl::put_event (unsigned int evid, unsigned int incr)
{
    int r = NO_ERROR;
    assert (incr);
    lock ();
    if ((evid >= N_MQ) && (evid < N_MQ + N_EC))
    {
        _ecnt [evid - N_MQ] += incr;
        eput (evid);
    }
    else  r = BAD_PORT;
    unlock ();
    return r;
}


inline int ITC_ctrl::put_event (unsigned int evid, ITC_mesg *M)
{
    int r = NO_ERROR;
    assert (M);
    lock ();
    if (evid < N_MQ)
    {
        _list [evid].put (M);
        eput (evid);
    }
    else r = BAD_PORT;
    unlock ();
    return r;
}


inline void ITC_ctrl::ipflush (unsigned int evid)
{
    lock ();
    if (evid < N_MQ) _list [evid].flush ();
    else if (evid < N_MQ + N_EC) _ecnt [evid - N_MQ] = 0;
    unlock ();
}


inline int ITC_ctrl::find_event (unsigned int mask) const
{
    int          i;
    unsigned int b;

    for (b = mask & EM_EC, i = N_EC - 1; b; b <<= 1, i--)
    {
        if ((b & 0x80000000) && _ecnt [i]) return i + N_MQ;
    }
    mask <<= N_EC;
    for (b = mask & (EM_MQ << N_EC), i = N_MQ - 1; b; b <<= 1, i--)
    {
        if ((b & 0x80000000) && _list [i].head ()) return i;
    }

    return EV_TIME;
}


// -------------------------------------------------------------------------------------------

/**
 * Basic thread
 */
class P_thread
{
public:

    P_thread ();
    virtual ~P_thread ();
    P_thread (const P_thread&);
    P_thread& operator=(const P_thread&);

    void sepuku () { pthread_cancel (_ident); }

    /**
     * Main thread loop, override in daughter classes to do something useful
     */
    virtual void thr_main () {};
    /**
     * Start the thread
     * @param policy Thread policy. See https://android.googlesource.com/platform/bionic/+/master/libc/kernel/, sched.h
     *               If you don't know, provide SCHED_NORMAL
     * @param priority Thread priority. If you don't know, provide 0.
     * @param stacksize Thread stack size. If you don't have specific reasons, provide 0
     * @return
     */
    virtual int  thr_start (int policy, int priority, size_t stacksize = 0);

private:

    pthread_t  _ident; // Internal information about this thread
};


// -------------------------------------------------------------------------------------------

/** Application helper thread, with a single input port and an internal queue */
class H_thread : public P_thread, public ITC_ip1q
{
public:

    H_thread (Edest *dest, int ipid) : _dest (dest), _ipid (ipid) {}
    virtual ~H_thread (void) {};
    H_thread (const H_thread&);
    H_thread& operator=(const H_thread&);

    void reply (ITC_mesg *M) { _dest->put_event (_ipid, M); }
    void reply (void) { _dest->put_event (_ipid, 1); }

private:

    Edest *_dest;
    int    _ipid;
};


// -------------------------------------------------------------------------------------------

/** @brief Application thread with multiple input and output ports
 *
 * This class combines threading capabilities with inter-thread communication control.
 */
class A_thread : public P_thread, public ITC_ctrl
{
public:

    explicit A_thread (const char *name);
    ~A_thread () override =default;
    A_thread (const A_thread&);
    A_thread& operator=(const A_thread&);

    void mprintf (int opid, const char *fmt, ...);
    /**
    * inst() - Get the instance number of this thread
    * @return: Integer instance identifier
    *
    * Returns the unique instance number assigned to this thread,
    * useful for distinguishing between multiple threads of the same type.
    */
    int inst () { return _inst; }
    const char *name () { return _name; }

    static unsigned long _trace;

private:

    char    _name [32]={0};
    int     _inst;
};


// -------------------------------------------------------------------------------------------


class Textmsg : public ITC_mesg
{
public:

    enum
    {
        ITM_CLLIB_TEXT = ITM_CLLIB_BASE + 1
    };

    Textmsg (size_t size);
    ~Textmsg (void) { delete _text; _counter--; }
    Textmsg (const Textmsg&);
    Textmsg& operator= (const Textmsg&);

    char *text (void) const { return _text; }
    size_t size (void) const { return _size; }
    size_t strlen (void) const { return _strlen; }
    int count (void) const { return _count; }

    virtual void recover (void) { delete this; }

    int set_count (int k) { return _count = k; }
    int inc_count (void) { return ++_count; }
    int dec_count (void) { return --_count; }
    void reset (void) { _strlen = 0; _count = 0; _lp = 0; _lc = 0; }

    void vprintf (const char *fmt, va_list ap);
    void printf (const char *fmt, ...);

    const char *getword (void);
    const char *gettail (void);
    void restore (void);

    static size_t object_counter (void) { return _counter; }

private:

    char    *_text;
    size_t   _size;
    size_t   _strlen;
    int      _count;
    char    *_lp;
    char     _lc;

    static size_t _counter;
};


// -------------------------------------------------------------------------------------------


#endif
