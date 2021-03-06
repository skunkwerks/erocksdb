// -------------------------------------------------------------------
//
// eleveldb: Erlang Wrapper for LevelDB (http://code.google.com/p/leveldb/)
//
// Copyright (c) 2011-2013 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// -------------------------------------------------------------------

#ifndef INCL_REFOBJECTS_H
#define INCL_REFOBJECTS_H

#include <stdint.h>
#include <list>

#include "rocksdb/db.h"
#include "rocksdb/write_batch.h"

#ifndef INCL_THREADING_H
    #include "threading.h"
#endif

#ifndef __WORK_RESULT_HPP
    #include "work_result.hpp"
#endif

#ifndef ATOMS_H
    #include "atoms.h"
#endif


namespace erocksdb {

/**
 * Base class for any object that offers RefInc / RefDec interface
 */

class RefObject
{
public:

protected:
    volatile uint32_t m_RefCount;     //!< simple count of reference, auto delete at zero

public:
    RefObject();

    virtual ~RefObject();

    virtual uint32_t RefInc();

    virtual uint32_t RefDec();

private:
    RefObject(const RefObject&);              // nocopy
    RefObject& operator=(const RefObject&);   // nocopyassign
};  // class RefObject


/**
 * Base class for any object that is managed as an Erlang reference
 */

class ErlRefObject : public RefObject
{
public:
    // these member objects are public to simplify
    //  access by statics and external APIs
    //  (yes, wrapper functions would be welcome)
    volatile uint32_t m_CloseRequested;  // 1 once api close called, 2 once thread starts destructor, 3 destructor done

    // DO NOT USE CONTAINER OBJECTS
    //  ... these must be live after destructor called
    pthread_mutex_t m_CloseMutex;        //!< for erlang forced close
    pthread_cond_t  m_CloseCond;         //!< for erlang forced close

protected:


public:
    ErlRefObject();

    virtual ~ErlRefObject();

    virtual uint32_t RefDec();

    // allows for secondary close actions IF InitiateCloseRequest returns true
    virtual void Shutdown()=0;

    // the following will sometimes be called AFTER the
    //  destructor ... in which case the vtable is not valid
    static bool InitiateCloseRequest(ErlRefObject * Object);

    static void AwaitCloseAndDestructor(ErlRefObject * Object);


private:
    ErlRefObject(const ErlRefObject&);              // nocopy
    ErlRefObject& operator=(const ErlRefObject&);   // nocopyassign
};  // class RefObject


/**
 * Class to manage access and counting of references
 * to a reference object.
 */

template <class TargetT>
class ReferencePtr
{
    TargetT * t;

public:
    ReferencePtr()
        : t(NULL)
    {};

    ReferencePtr(TargetT *_t)
        : t(_t)
    {
        if (NULL!=t)
            t->RefInc();
    }

    ReferencePtr(const ReferencePtr &rhs)
    {t=rhs.t; if (NULL!=t) t->RefInc();};

    ~ReferencePtr()
    {
        if (NULL!=t)
            t->RefDec();
    }

    void assign(TargetT * _t)
    {
        if (_t!=t)
        {
            if (NULL!=t)
                t->RefDec();
            t=_t;
            if (NULL!=t)
                t->RefInc();
        }   // if
    };

    TargetT * get() {return(t);};

    TargetT * operator->() {return(t);};

private:
 ReferencePtr & operator=(const ReferencePtr & rhs); // no assignment

};  // ReferencePtr


/**
 * Per database object.  Created as erlang reference.
 *
 * Extra reference count created upon initialization, released on close.
 */
class DbObject : public ErlRefObject
{
public:
    rocksdb::DB* m_Db;                                   // NULL or rocksdb database object

    rocksdb::Options *m_DbOptions;

    Mutex m_ItrMutex;                         //!< mutex protecting m_ItrList
    std::list<class ItrObject *> m_ItrList;   //!< ItrObjects holding ref count to this

protected:
    static ErlNifResourceType* m_Db_RESOURCE;

public:
    DbObject(rocksdb::DB * DbPtr, rocksdb::Options * Options); // Open with default CF

    virtual ~DbObject();

    virtual void Shutdown();

    // manual back link to ItrObjects holding reference to this
    void AddReference(class ItrObject *);

    void RemoveReference(class ItrObject *);

    static void CreateDbObjectType(ErlNifEnv * Env);

    static DbObject * CreateDbObject(rocksdb::DB * Db, rocksdb::Options* Options);

    static DbObject * RetrieveDbObject(ErlNifEnv * Env, const ERL_NIF_TERM & DbTerm);

    static void DbObjectResourceCleanup(ErlNifEnv *Env, void * Arg);

private:
    DbObject();
    DbObject(const DbObject&);              // nocopy
    DbObject& operator=(const DbObject&);   // nocopyassign
};  // class DbObject


/**
 * A self deleting wrapper to contain rocksdb snapshot pointer.
 *   Needed because multiple RocksIteratorWrappers could be using
 *   it ... and finishing at different times.
 */

class RocksSnapshotWrapper : public RefObject
{
public:
    ReferencePtr<DbObject> m_DbPtr;  //!< need to keep db open for delete of this object
    const rocksdb::Snapshot * m_Snapshot;

    // this is an odd place to put this info, but it
    //  happens to have the exact same lifespan
    ERL_NIF_TERM itr_ref;
    ErlNifEnv *itr_ref_env;

    RocksSnapshotWrapper(DbObject * DbPtr, const rocksdb::Snapshot * Snapshot)
        : m_DbPtr(DbPtr), m_Snapshot(Snapshot), itr_ref_env(NULL)
    {
    };

    virtual ~RocksSnapshotWrapper()
    {
        if (NULL!=itr_ref_env)
            enif_free_env(itr_ref_env);

        if (NULL!=m_Snapshot)
        {
            // rocksdb performs actual "delete" call on m_Shapshot's pointer
            m_DbPtr->m_Db->ReleaseSnapshot(m_Snapshot);
            m_Snapshot=NULL;
        }   // if
    }   // ~RocksSnapshotWrapper

    const rocksdb::Snapshot * get() {return(m_Snapshot);};
    const rocksdb::Snapshot * operator->() {return(m_Snapshot);};

private:
    RocksSnapshotWrapper(const RocksSnapshotWrapper &);            // no copy
    RocksSnapshotWrapper& operator=(const RocksSnapshotWrapper &); // no assignment

};  // RocksSnapshotWrapper



/**
 * A self deleting wrapper to contain rocksdb iterator.
 *   Used when an ItrObject needs to skip around and might
 *   have a background MoveItem performing a prefetch on existing
 *   iterator.
 */

class RocksIteratorWrapper : public RefObject
{
public:
    ReferencePtr<DbObject> m_DbPtr;           //!< need to keep db open for delete of this object
    ReferencePtr<RocksSnapshotWrapper> m_Snap;//!< keep snapshot active while this object is
    rocksdb::Iterator * m_Iterator;
    volatile uint32_t m_HandoffAtomic;        //!< matthew's atomic foreground/background prefetch flag.
    bool m_KeysOnly;                          //!< only return key values
    bool m_PrefetchStarted;                   //!< true after first prefetch command

    RocksIteratorWrapper(DbObject * DbPtr, RocksSnapshotWrapper * Snapshot,
                         rocksdb::Iterator * Iterator, bool KeysOnly)
        : m_DbPtr(DbPtr), m_Snap(Snapshot), m_Iterator(Iterator),
        m_HandoffAtomic(0), m_KeysOnly(KeysOnly), m_PrefetchStarted(false)
    {
    };

    virtual ~RocksIteratorWrapper()
    {
        if (NULL!=m_Iterator)
        {
            delete m_Iterator;
            m_Iterator=NULL;
        }   // if
    }   // ~RocksIteratorWrapper

    rocksdb::Iterator * get() {return(m_Iterator);};
    rocksdb::Iterator * operator->() {return(m_Iterator);};

    bool Valid() {return(m_Iterator->Valid());};
    rocksdb::Slice key() {return(m_Iterator->key());};
    rocksdb::Slice value() {return(m_Iterator->value());};

private:
    RocksIteratorWrapper(const RocksIteratorWrapper &);            // no copy
    RocksIteratorWrapper& operator=(const RocksIteratorWrapper &); // no assignment


};  // RocksIteratorWrapper



/**
 * Per Iterator object.  Created as erlang reference.
 */
class ItrObject : public ErlRefObject
{
public:
    ReferencePtr<RocksIteratorWrapper> m_Iter;
    ReferencePtr<RocksSnapshotWrapper> m_Snapshot;

    bool keys_only;
    rocksdb::ReadOptions * m_ReadOptions;

    volatile class MoveTask * reuse_move;//!< iterator work object that is reused instead of lots malloc/free

    ReferencePtr<DbObject> m_DbPtr;

protected:
    static ErlNifResourceType* m_Itr_RESOURCE;

public:
    ItrObject(DbObject *, bool, rocksdb::ReadOptions *);

    virtual ~ItrObject(); // needs to perform free_itr

    virtual void Shutdown();

    static void CreateItrObjectType(ErlNifEnv * Env);

    static ItrObject * CreateItrObject(DbObject * Db, bool KeysOnly, rocksdb::ReadOptions * Options);

    static ItrObject * RetrieveItrObject(ErlNifEnv * Env, const ERL_NIF_TERM & DbTerm,
                                         bool ItrClosing=false);

    static void ItrObjectResourceCleanup(ErlNifEnv *Env, void * Arg);

    bool ReleaseReuseMove();

private:
    ItrObject();
    ItrObject(const ItrObject &);            // no copy
    ItrObject & operator=(const ItrObject &); // no assignment
};  // class ItrObject

} // namespace erocksdb


#endif  // INCL_REFOBJECTS_H
