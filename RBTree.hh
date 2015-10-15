#pragma once

#include <cassert> 
#include <utility>
#include "TaggedLow.hh"
#include "Transaction.hh"
#include "versioned_value.hh"
#include "VersionFunctions.hh"
#include "RBTreeInternal.hh"

template <typename K, typename T>
class RBTree;

template <typename T>
class rbwrapper : public T {
  public:
    template <typename... Args> inline rbwrapper(Args&&... args)
    : T(std::forward<Args>(args)...) {
    }
    inline rbwrapper(const T& x)
    : T(x) {
    }
    inline rbwrapper(T&& x) noexcept
    : T(std::move(x)) {
    }
    inline const T& value() const {
        return *this;
    }
    inline T& mutable_value() {
        return *this;
    }
    rblinks<rbwrapper<T> > rblinks_;
};

// Define a custom key-value pair type that contains versions and also
// has proper comparison methods
template <typename K, typename T>
class rbpair {
public:
    typedef TransactionTid::type Version;
    typedef versioned_value_struct<std::pair<K, T>> versioned_pair;

    static constexpr Version insert_bit = TransactionTid::user_bit1;

    explicit rbpair(const K& key, const T& value) :
        pair_(std::pair<K, T>(key, value),
              TransactionTid::increment_value + insert_bit) {}
    explicit rbpair(std::pair<K, T>& kvp) :
        pair_(kvp, TransactionTid::increment_value + insert_bit) {}

    inline const K& key() const {
        return pair_.read_value().first;
    }
    inline Version& version() {
        return pair_.version();
    }
    inline Version& nodeversion() {
        return nodeversion_;
    }
    inline T& writeable_value() {
        return pair_.writeable_value().second;
    }
    inline bool operator<(const rbpair& rhs) const {
        return (key() < rhs.key());
    }
    inline bool operator>(const rbpair& rhs) const {
        return (key() > rhs.key());
    }

private:
    // key-value pair associated with a version for the data
    versioned_pair pair_;
    // version associated with the structure of the tree / node placement in tree
    Version nodeversion_;
};

template <typename K, typename T>
class RBProxy;

template <typename K, typename T>
class RBTree : public Shared {
    typedef TransactionTid::type Version;
    typedef versioned_value_struct<T> versioned_value;

    static constexpr TransItem::flags_type insert_tag = TransItem::user0_bit;
    static constexpr TransItem::flags_type delete_tag = TransItem::user0_bit<<1;
    static constexpr TransItem::flags_type structure_tag = TransItem::user0_bit<<2;
    static constexpr Version insert_bit = TransactionTid::user_bit1;
    static constexpr Version delete_bit = TransactionTid::user_bit1<<1;

public:
    RBTree() {
        treelock_ = 0;
        treeversion_ = 0;
#if PRINT_DEBUG
        stats_ = {0,0,0,0,0,0};
#endif
    }
    
    typedef rbwrapper<rbpair<K, T>> wrapper_type;
    typedef rbtree<wrapper_type> internal_tree_type;
    
    // lookup
    inline size_t count(const K& key) const;
    // element access
    inline RBProxy<K, T> operator[](const K& key);
    // modifiers
    inline size_t erase(const K& key);

    void lock(versioned_value *e) {
        lock(&e->version());
    }
    void unlock(versioned_value *e) {
        unlock(&e->version());
    }

    inline void lock(TransItem& item);
    inline void unlock(TransItem& item);
    inline bool check(const TransItem& item, const Transaction& trans);
    inline void install(TransItem& item, const Transaction& t);
    inline void cleanup(TransItem& item, bool committed);
#if PRINT_DEBUG
    inline void print_absent_reads();
#endif

private:
    inline size_t debug_size() const {
        return wrapper_tree_.size();
    }

    // Find and return a pointer to the rbwrapper. Abort if value inserted and not yet committed.
    inline std::pair<rbwrapper<rbpair<K, T>>*, rbwrapper<rbpair<K, T>>*> find_or_abort(rbwrapper<rbpair<K, T>>& rbkvp) const {
        lock(&treelock_);
        auto nodepair = wrapper_tree_.find_any(rbkvp,
            rbpriv::make_compare<wrapper_type, wrapper_type>(wrapper_tree_.r_.get_compare()));
        auto x = nodepair.first;
        if (x) {
            // check if rbpair has version "inserted" (someone inserted w/out commit)
            if (is_inserted(x->version())) {
                // XXX const_cast hack here... any other way to do this?
                auto item = Sto::item(const_cast<RBTree<K, T>*>(this), x);
                // check if item was inserted by this transaction
                if (has_insert(item)) {
                    unlock(&treelock_);
                    return nodepair;
                } else {
                    // some other transaction inserted this node and hasn't committed
                    unlock(&treelock_);
                    Sto::abort();
                    // unreachable
                    return nodepair; 
                }
            }
        }
        // item was committed or DNE, so return pointer
        unlock(&treelock_);
        return nodepair;
    }

    // Insert <@key, @value>, optionally force an update if @key exists
    // return value is a reference to kvp.second
    inline T& insert_update(const K& key, const T& value, bool force) {
        auto node = rbwrapper<rbpair<K, T>>( rbpair<K, T>(key, T()) );
        auto nodepair = this->find_or_abort(node);
        auto x = nodepair.first;
        lock(&treelock_);
        
        // INSERT: kvp did not exist
        if (!x) {
#if PRINT_DEBUG
            stats_.absent_insert++;
#endif
            auto n = new rbwrapper<rbpair<K, T> >(  rbpair<K, T>(key, value)  );
            wrapper_tree_.insert(*n);
            // invariant: the node's insert_bit should be set
            assert(is_inserted(n->version()));
            // we will increment parent nodeversion upon commit (XXX will parent be the same?)
            // if tree is empty (i.e. no parent), we increment treeversion 
            if (!nodepair.second) {
                Sto::item(this, tree_key_).add_write(0);
            // else we increment the parent version 
            } else {
                Sto::item(this, nodepair.second).add_write(0).add_flags(structure_tag);
            }
            // add write and insert flag of item (value of rbpair) with @value
            Sto::item(this, n).add_write(value).add_flags(insert_tag);
            unlock(&treelock_);
            return n->writeable_value();

        // UPDATE: kvp is already inserted into the tree
        } else {
#if PRINT_DEBUG
            stats_.present_insert++;
#endif
            auto item = Sto::item(this, x);
            if (is_deleted(x->version())) {
                unlock(&treelock_);
                Sto::abort();
                // should be unreachable
                return x->writeable_value();
            } else if (has_delete(item)) {
                // we only reach here if read_my_delete; always doing an insert due to
                // operator[] semantics
                // add item value to write set and marked as inserted instead of deleted
                // insert_tag indicates that we need to remove the insert_bit during install
                item.add_write(value).clear_flags(delete_tag).add_flags(insert_tag);
                // either overwrite value or put empty value
                if (force) {
                    x->writeable_value() = value;
                } else {
                    x->writeable_value() = T();
                }
                unlock(&treelock_);
                return x->writeable_value();
            } else if (item.has_write()) {
                // read_my_writes: don't add anything to read set
                if (force) {
                    // operator[] used on LHS, overwrite
                    item.add_write(value);
                }
                unlock(&treelock_);
                return item.template write_value<T>();
            }

            // otherwise we are just accessing a regular key
            // either overwrite value or put empty value
            if (force) {
                item.add_write(value);
            } else {
                item.add_read(x->version());
            }
            unlock(&treelock_);
            return x->writeable_value();
        }
    }

    static void lock(Version *v) {
        TransactionTid::lock(*v);
    }
    static void unlock(Version *v) {
        TransactionTid::unlock(*v);
    }
    static bool has_structure(const TransItem& item) {
        return item.flags() & structure_tag;
    }
    static bool has_insert(const TransItem& item) {
        return item.flags() & insert_tag;
    }
    static bool has_delete(const TransItem& item) {
        return item.flags() & delete_tag;
    }
    static bool is_locked(Version v) {
        return TransactionTid::is_locked(v);
    }
    static bool is_inserted(Version v) {
        return v & insert_bit;
    }
    static void erase_inserted(Version* v) {
        *v = *v & (~insert_bit);
    }
    static void mark_inserted(Version* v) {
        *v = *v | insert_bit;
    }
    static bool is_deleted(Version v) {
        return v & delete_bit;
    }
    static void erase_deleted(Version* v) {
        *v = *v & (~delete_bit);
    }
    static void mark_deleted(Version* v) {
        *v = *v | delete_bit;
    }

    internal_tree_type wrapper_tree_;
    mutable Version treelock_;
    mutable Version treeversion_;
#if PRINT_DEBUG
    mutable struct stats {
        int absent_insert, absent_delete, absent_count;
        int present_insert, present_delete, present_count;
    } stats_;
#endif
    // used to mark whether a key is for the tree structure (for tree version checks)
    // or a pointer (which will always have the lower 3 bits as 0)
    static constexpr uintptr_t tree_bit = 1U<<0;
    void* tree_key_ = (void*) tree_bit;
 
    friend class RBProxy<K, T>;
};

// STL-ish interface wrapper returned by RBTree::operator[]
template <typename K, typename T>
class RBProxy {
public:
    typedef RBTree<K, T> transtree_t;
    explicit RBProxy(transtree_t& tree, const K& key)
        : tree_(tree), key_(key) {};
    operator T() {
        return tree_.insert_update(key_, T(), false);
    }
    RBProxy& operator=(const T& value) {
        tree_.insert_update(key_, value, true);
        return *this;
    };
    RBProxy& operator=(RBProxy& other) {
        tree_.insert_update(key_, (T)other, true);
        return *this;
    };
private:
    transtree_t& tree_;
    const K& key_;
};

template <typename K, typename T>
inline size_t RBTree<K, T>::count(const K& key) const {
    rbwrapper<rbpair<K, T>> idx_pair(rbpair<K, T>(key, T()));
    auto nodepair = find_or_abort(idx_pair); // return both parent and child
    auto x = nodepair.first;
#if PRINT_DEBUG
    (!x) ? stats_.absent_count++ : stats_.present_count++;
#endif
    // we will check parent version upon commit if absent read
    if (!x) {
        if (!nodepair.second) {
            Sto::item(const_cast<RBTree<K, T>*>(this), tree_key_).add_read(treeversion_);
        } else {
            Sto::item(const_cast<RBTree<K, T>*>(this), nodepair.second).add_read(nodepair.second->nodeversion());
        }
        return 0;
    // else we found the item, check the item version at commit time
    } else  {
        Sto::item(const_cast<RBTree<K, T>*>(this), x).add_read(x->version());
        return 1;
    }
}

template <typename K, typename T>
inline RBProxy<K, T> RBTree<K, T>::operator[](const K& key) {
    return RBProxy<K, T>(*this, key);
}

template <typename K, typename T>
inline size_t RBTree<K, T>::erase(const K& key) {
    rbwrapper<rbpair<K, T>> idx_pair(rbpair<K, T>(key, T()));
    auto nodepair = find_or_abort(idx_pair);
    auto x = nodepair.first;
    // FOUND ITEM
    if (x) {
#if PRINT_DEBUG
        stats_.present_delete++;
#endif
        auto item = Sto::item(this, x);
        // item marked as inserted and not yet installed
        if (is_inserted(x->version())) {
            if (has_insert(item)) {
                // insert-then-delete; we need to undo all the effects of an insert here
                lock(&treelock_);
                wrapper_tree_.erase(*x);
                item.remove_read().remove_write().clear_flags(insert_tag | delete_tag);
                Transaction::rcu_free(x);
                // read a parentversion so that we can abort if another transaction inserts
                // this key again
                if (!nodepair.second) {
                    Sto::item(this, tree_key_).add_read(treeversion_);
                } else {
                    Sto::item(const_cast<RBTree<K, T>*>(this), nodepair.second).add_read(nodepair.second->nodeversion());
                }
                unlock(&treelock_);
                return 1;
            } else {
                Sto::abort();
                // unreachable
                return 0;
            }
        }
        // item marked at install as deleted, not deleted yet so abort
        if (is_deleted(x->version())) {
            Sto::abort();
            return 0;
        }
        // found item that has already been installed and not deleted
        item.add_write(0).add_flags(delete_tag);
        item.add_read(x->version());
        // must change the parent node version
        // if tree is empty (i.e. no parent), we increment treeversion 
        if (!nodepair.second) {
            Sto::item(this, tree_key_).add_write(0);
        // else we increment the parent version
        } else {
            Sto::item(this, nodepair.second).add_write(0).add_flags(structure_tag);
        }
        return 1;
    // item not in tree, treat it like an absent read
    } else {
#if PRINT_DEBUG
        stats_.absent_delete++;
#endif
        if (!nodepair.second) {
            Sto::item(this, tree_key_).add_read(treeversion_);
        } else {
            Sto::item(const_cast<RBTree<K, T>*>(this), nodepair.second).add_read(nodepair.second->nodeversion());
        }
        return 0;
    }
}

template <typename K, typename T>
inline void RBTree<K, T>::lock(TransItem& item) {
    if (item.key<void*>() == tree_key_) {
        lock(&treeversion_);
    } else if (has_structure(item)) {
        lock(&(item.key<wrapper_type*>()->nodeversion()));
    } else {
        lock(item.key<versioned_value*>());
    }
}
    
template <typename K, typename T>
inline void RBTree<K, T>::unlock(TransItem& item) {
    if (item.key<void*>() == tree_key_) {
        unlock(&treeversion_);
    } else if (has_structure(item)) {
        unlock(&(item.key<wrapper_type*>()->nodeversion()));
    } else {
        unlock(item.key<versioned_value*>());
    }
}
   
template <typename K, typename T>
inline bool RBTree<K, T>::check(const TransItem& item, const Transaction& trans) {
    auto e = item.key<wrapper_type*>();
    auto read_version = item.read_value<Version>();
    Version curr_version;

    // set up the correct current version to check: either treeversion, item version, or nodeversion
    if (e == (wrapper_type*)tree_key_)
        curr_version = treeversion_;
    else if (has_structure(item))
        curr_version = e->nodeversion();
    else
        curr_version = e->version();

    bool same_version = (read_version ^ curr_version) <= TransactionTid::lock_bit;
    bool not_locked = (!is_locked(curr_version) || item.has_lock(trans));
    // to handle the case that we check nodeversion, but the node (parent) is deleted
    bool not_deleted = !is_deleted(curr_version);
    return same_version && not_locked && not_deleted;
}

// key-versionedvalue pairs with the same key will have two different items
template <typename K, typename T>
inline void RBTree<K, T>::install(TransItem& item, const Transaction& t) {
    (void) t;
    auto e = item.key<wrapper_type*>();
    if (e != (wrapper_type*)tree_key_) {
        assert(is_locked(e->version()) || is_locked(e->nodeversion()));

        bool deleted = has_delete(item);
        bool inserted = has_insert(item);
        bool structured = has_structure(item);
        // should never be both deleted and inserted...
        // sanity check to make sure we handled read_my_writes correctly
        assert(!(deleted && inserted));
        // actually erase the element when installing the delete
        if (deleted) {
            // mark deleted (in case rcu free doesn't free immediately)
            mark_deleted(&e->version());
            // actually erase
            lock(&treelock_);
            wrapper_tree_.erase(*e);
            unlock(&treelock_);
            Transaction::rcu_free(e);
        } else if (inserted) {
            erase_inserted(&e->version());
        // we made a structural change to this item (changed its children)
        // update the nodeversion of the item
        } else if (structured) {
            TransactionTid::inc_invalid_version(e->nodeversion());
        // item in write set did not update value --> read_my_writes 
        } else {
            // XXX what is valid bit?
            e->writeable_value() = item.template write_value<T>(); 
            TransactionTid::inc_invalid_version(e->version());
        }
    // we did something to an empty tree, so update treeversion
    } else {
        assert(is_locked(treeversion_));
        TransactionTid::inc_invalid_version(treeversion_);
    }
}

template <typename K, typename T>
inline void RBTree<K, T>::cleanup(TransItem& item, bool committed) {
    if (!committed) {
        // if item has been tagged deleted or structured, don't need to do anything 
        // if item has been tagged inserted, then we erase the item
        if (has_insert(item)) {
            auto e = item.key<wrapper_type*>();
            // we actually want to erase this 
            lock(&treelock_);
            wrapper_tree_.erase(*e);
            unlock(&treelock_);
            mark_deleted(&e->version());
            erase_inserted(&e->version());
            Transaction::rcu_free(e);
        }
    }
}

#if PRINT_DEBUG 
template <typename K, typename T>
inline void RBTree<K, T>::print_absent_reads() {
    std::cout << "absent inserts: " << stats_.absent_insert << std::endl;
    std::cout << "absent deletes: " << stats_.absent_delete << std::endl;
    std::cout << "absent counts: " << stats_.absent_count << std::endl;
    std::cout << "present inserts: " << stats_.present_insert << std::endl;
    std::cout << "present deletes: " << stats_.present_delete << std::endl;
    std::cout << "present counts: " << stats_.present_count << std::endl;
    std::cout << "size: " << debug_size() << std::endl;
}
#endif
