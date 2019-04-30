#ifndef _MY_CACHE_H_
#define _MY_CACHE_H_

#define DEBUG_LXH_CACHE
#ifdef DEBUG_LXH_CACHE
#define PRINTF printf
#else
#define PRINTF
#endif

#include <functional>
#include <assert.h>
#include <type_traits>
#include <iostream>
#include <memory>
#include <utility>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <mutex>

#ifndef u32
#define u32 unsigned int
#endif

namespace lxh {
namespace cache {

//缓存的数据的类型
static const int node_type_default         = 0;
static const int node_type_hit_count       = 1;
static const int node_type_expire_time     = 2;
static const int node_type_expire_callback = 4;

enum class CacheType {
    LRU = 0,
    FIFO = 1
};

enum class RunStatus {
    ORIG = 0, //原始状态
    INIT,     //初始化状态,申请资源
    RUNNING,  //运行中
    STOP,     //结束
    ERROR     //错误
};

class LRUCacheTimeThread {
public:
    static LRUCacheTimeThread & GetInstance() {
        static LRUCacheTimeThread t;
        return t;
    }
    const long & GetCurSecond() const { return g_sys_cur_time_; }
public:
    LRUCacheTimeThread() {
        long & t = g_sys_cur_time_;
        std::thread th([&t]() {
            while(1) {
                t = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                PRINTF("t: %ld; g_sys_cur_time_: %ld\n", t, LRUCacheTimeThread::GetInstance().GetCurSecond());
                //sleep(1);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
        th.detach();
    }
private:
    long g_sys_cur_time_ = 0;
};

template<typename Key, typename Value, CacheType ct = CacheType::LRU> //, bool with_hit_count = false, bool with_expire_time = false>
class MyCache {
public:
    class Node;
    typedef std::function<void()> ExpireCallbackFun;
    using map_type = std::unordered_map<Key, Node*>;
public:
    MyCache(const int max_size) : hashmap_(max_size * 2) {
        assert(max_size > 1);
        max_size_ = max_size;
    }
    MyCache &with_hit_count() {
        if(run_status_ == RunStatus::ORIG) with_hit_count_ = true;
        return *this;
    }
    MyCache &with_expire_time(const long expire_time, ExpireCallbackFun * expire_callback_fun = NULL, const int batch_size = 10) {
        if(run_status_ == RunStatus::ORIG && expire_time > 0) {
            with_expire_time_ = true;
            expire_time_ = expire_time;
            expire_callback_fun_ = expire_callback_fun;
            max_callback_batch_size_ = batch_size;
            LRUCacheTimeThread::GetInstance();
            if(expire_callback_fun) {
                std::thread th(&MyCache::ExpireCheck, this);
                th.detach();
            }
        }
        return *this;
    }
    bool Start() {
        if(run_status_ != RunStatus::ORIG) return false;
        run_status_ = RunStatus::INIT;
        bool ret = InitResource();
        if(ret) {
            run_status_ = RunStatus::RUNNING;
        } else {
            run_status_ = RunStatus::ERROR;
        }
        return ret;
    };
    void Put(Key key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        DoPut(key, value);
    }
    std::pair<Value, bool> Get(Key key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return DoGet(key);
    }
    ~MyCache() {
        run_status_ = RunStatus::STOP;
        if(nodes_array_) {
            delete []nodes_array_;
            nodes_array_ = NULL;
        }
    }
    void Debug() {
        DoDebug();
    }
private:
    void DoDebug() {
#ifdef DEBUG_LXH_CACHE
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "hashmap data: " << std::endl;
        for(auto it = hashmap_.begin(); it != hashmap_.end(); ++it) {
            std::cout << it->first << " ";
        }
        std::cout << std::endl;
        std::cout << "node_list: " << std::endl;
        const long cur_time = LRUCacheTimeThread::GetInstance().GetCurSecond();
        for(auto t = list_head_; t; t = t->next) {
            std::cout << t->data;
            if(cur_time > t->ts) {
                std::cout << "[drop] ";
            } else {
                std::cout << " ";
            }
        }
        std::cout << std::endl;
#endif
    }
    std::pair<Value, bool> DoGet(Key key) {
        static const Value const_v;
        auto it = hashmap_.find(key);
        if (it == hashmap_.end()) {
            ++miss_count_;
            return std::pair<Value, bool>(const_v, false);
        }

        const long cur_time = LRUCacheTimeThread::GetInstance().GetCurSecond();
        Node* node = it->second;
        node->hit_count++;
        if(node->ts < cur_time) {
            ++overtime_count_;
            return std::pair<Value, bool>(const_v, false);
        }
        hit_count_++;

        if(ct == CacheType::LRU) {
            node->ts = cur_time + expire_time_;
            BumpToFront(node);
        }

        return std::pair<Value, bool>(node->data, true);
    }
    void DoPut(Key key, Value value) {
        Node* new_node = NewNode(value);
        auto it = hashmap_.insert(typename map_type::value_type(key, new_node));
        if (!it.second) { //已经存在
            PRINTF("add a node, this node has exist\n");
            //跟老得节点具有相同的prev&next
            new_node->prev = it.first->second->prev;
            new_node->next = it.first->second->next;
            new_node->map_it = it.first->second->map_it;
            if(list_head_ == it.first->second) list_head_ = new_node; //之前是head,head指向新的节点
            if(list_tail_ == it.first->second) list_tail_ = new_node; //之前是tail,tail指向新节点
            if(it.first->second->prev) it.first->second->prev->next = new_node;
            if(it.first->second->next) it.first->second->next->prev = new_node;
            DeleteNode(it.first->second); //删除老的
            it.first->second = new_node;
            BumpToFront(it.first->second); //注意: 如果新插入的,把它放到最前面(因为为最新)
            return;
        }
        PRINTF("add a new node\n");

        //之前不存在，插入新的元素
        new_node->map_it = it.first;
        if (list_head_ == NULL) {
            list_head_ = list_tail_ = new_node;
        } else {
            list_head_->prev = new_node;
            new_node->next = list_head_;
            list_head_ = new_node;
        }

        size_++;

        if (size_ > max_size_) { //没有空闲
            PRINTF("need delete old node; size_: %d; max_size_: %d\n", size_, max_size_);
            hashmap_.erase(list_tail_->map_it);
            list_tail_ = list_tail_->prev;
            DeleteNode(list_tail_->next);
            list_tail_->next = NULL;
            size_--;
        }
    }
    void BumpToFront(Node* node) {
        PRINTF("BumpToFront\n");
        if (list_head_ == node)
            return;

        PRINTF("=>BumpToFront: more than one node\n");
        list_head_->prev = node;

        if (node != list_tail_)
            node->next->prev = node->prev;
        else
            list_tail_ = list_tail_->prev;

        node->prev->next = node->next;

        node->prev = NULL;
        node->next = list_head_;

        list_head_ = node;
    }
    void DeleteNode(Node * node) {
        PRINTF("DeleteNode\n");
        if(free_node_tail_ == NULL) {
            PRINTF("=>DeleteNode: free_node_tail is empty\n");
            free_node_head_ = free_node_tail_ = node;
            node->next = NULL;
        } else {
            PRINTF("=>DeleteNode: free_node_tail not null\n");
            node->next = free_node_head_;
            free_node_head_ = node;
        }
    }
    Node * NewNode(Key key) {
        const long cur_time = LRUCacheTimeThread::GetInstance().GetCurSecond();
        long expire_time = cur_time + expire_time_;
        Node * node = NULL;
        if(free_node_head_ == free_node_tail_) {
            if(free_node_head_ == NULL) {
                node = new Node();
            } else {
                node = free_node_head_;
                free_node_head_ = NULL;
                free_node_tail_ = NULL;
            }
#ifdef DEBUG_LXH_CACHE
            std::cout << "NewNode: free list is empty: key: " << key << std::endl;
#endif
        } else {
            node = free_node_head_;
            free_node_head_ = free_node_head_->next;
        }
        node->Init(key, expire_time);
        return node;
    }
    bool InitResource() {
        nodes_array_ = new Node[max_size_ + 1]; //保证当前至少有一个可用
        free_node_head_ = nodes_array_ + 0;
        Node * t = free_node_head_;
        for(int i = 0; i < max_size_; ++i){
            t->next = nodes_array_ + i;
            nodes_array_[i].prev = t;
            t = nodes_array_ + i;
        }
        free_node_tail_ = t;
        return true;
    }
    void ExpireCheck() {
        while(1) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if(run_status_ == RunStatus::STOP) {
                break;
            }
            if(run_status_ != RunStatus::RUNNING) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
            }
        }
    }
    
public:
    class Node {
    public:
        Value data;
        u32 hit_count = 0;
        u32 ts = 0; //过期时间;   //drop: LRU: 最近更新时间; FIFO: 插入时间
        Node * prev = NULL;
        Node * next = NULL;
        typename map_type::iterator map_it;
    public:
        void Init(Value d, u32 t = 0l) {
            data = d;
            hit_count = 0;
            prev = NULL;
            next = NULL;
            ts = t;
        }
    };
private:
    RunStatus run_status_ = RunStatus::ORIG;
    bool with_hit_count_ = false;
    //过期时间校验
    ExpireCallbackFun * expire_callback_fun_ = NULL; //过期资源的回调函数
    bool with_expire_time_ = false; //是否需要过期时间校验
    int max_callback_batch_size_ = 0; //过期时间回调函数每次回调过期量的上限
    long expire_time_ = 0l; //生存时间,超过该时间认为过期
    Node * list_expire_ = NULL;

    std::mutex mutex_;
    map_type hashmap_;
    int max_size_;
    int size_ = 0;
    Node * list_head_ = NULL;
    Node * list_tail_ = NULL;
    Node * free_node_head_ = NULL;
    Node * free_node_tail_ = NULL;
    Node * nodes_array_ = NULL; //申请资源的地址
private: //统计数据
    long miss_count_ = 0l; //没有查询到的key次数
    long overtime_count_ = 0l; //查询已经超时的次数
    long hit_count_ = 0l; //命中次数
};


}; //cache
}; //lxh

#endif
