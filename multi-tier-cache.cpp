#include <iostream>
#include <optional>
#include <unordered_map>
#include <list>
#include <vector>
#include <mutex>
#include <string>
#include <stdexcept>
#include <shared_mutex>
#include <memory>
using namespace std;
template <typename K, typename V> struct EvictedEntry {
    K key;
    V value;
};

template <typename K, typename V> class ICacheTier {
    public:
        virtual ~ICacheTier() = default;
        virtual optional<V> get(const K& key) = 0;
        virtual bool remove(const K& key) = 0;

        virtual optional<EvictedEntry<K, V>> putAndReturnEvicted(const K& key, 
                            const V& value) = 0;
        virtual bool contains(const K& key)const = 0;
        virtual size_t size()const = 0;
        virtual size_t capacity()const = 0;

        virtual void debugPrint(ostream& os) const = 0;

};

template <typename K, typename V> class LRUCacheTier final : public ICacheTier<K, V> {
    private:
        size_t cap_;
        string name_;
        list<pair<K,V>> lru_;
        unordered_map<K, typename list<pair<K, V>>::iterator> map_;

    public:
        explicit LRUCacheTier(size_t cap, string name = "")
        : cap_(cap), name_(move(name)) {
            if(cap_ == 0){
                throw invalid_argument("Tier capacity must be > 0");
            }
        }

        optional<V> get(const K& key) override {
            auto it = map_.find(key);
            if(it == map_.end())
                return nullopt;
            
            lru_.splice(lru_.begin(), lru_, it->second);
            return it->second->second;
        }

        bool remove(const K& key) override {
            auto it = map_.find(key);
            if(it == map_.end())
                return false;
            lru_.erase(it->second);
            map_.erase(it);
            return true;
        }

        optional<EvictedEntry<K,V>> putAndReturnEvicted(const K& key, const V& value) override {
            auto it = map_.find(key);
            if(it != map_.end()){
                it->second->second = value;
                lru_.splice(lru_.begin(), lru_, it->second);
                return nullopt;
            }
            lru_.emplace_front(key, value);
            map_[key] = lru_.begin();

            if(map_.size() > cap_){
                auto tailIt = prev(lru_.end());
                EvictedEntry<K, V> ev {tailIt->first, tailIt->second};
                map_.erase(tailIt->first);
                lru_.erase(tailIt);
                return ev;
            }
            return nullopt;
        }
        
        bool contains(const K& key) const override{
            return map_.find(key)!=map_.end();
        }

        size_t size() const override {
            return map_.size();
        }

        size_t capacity() const override {
            return cap_;
        }
        void debugPrint(std::ostream& os) const override {
            os << "[" << (name_.empty() ? "tier" : name_) << " cap=" << cap_ << " size=" << map_.size() << "] ";
            os << "MRU -> ";
            for (auto it = lru_.begin(); it != lru_.end(); ++it) {
                os << "(" << it->first << ":" << it->second << ")";
                if (std::next(it) != lru_.end()) os << " ";
            }
            os << " <- LRU\n";
        }
    

};

template <typename K, typename V> class MultiTierCache {
    private: 
        vector<unique_ptr<ICacheTier<K, V>>> tiers_;
        mutable vector<mutex> tierLocks_;

        class LockAllTiers{
            private:
                vector<unique_lock<mutex>> locks_;
            public:
                explicit LockAllTiers(const MultiTierCache& cache){
                    locks_.reserve(cache.tierLocks_.size());
                    for(auto &m : cache.tierLocks_){
                        locks_.emplace_back(m);
                    }
                }
        };

            void insertWithCascade(size_t tierIndex, const K& key, const V& value){
                auto evicted = tiers_[tierIndex]->putAndReturnEvicted(key, value);
                if(!evicted.has_value())
                    return;
                if(tierIndex + 1 >= tiers_.size()){
                    return;
                }
                insertWithCascade(tierIndex + 1, evicted->key, evicted->value);
            }
        public:
            explicit MultiTierCache(vector<unique_ptr<ICacheTier<K, V>>> tiers): tiers_(move(tiers)), tierLocks_(tiers_.size()){
                if(tiers_.empty()){
                    throw std::invalid_argument("MultiTierCache needs at least one tier");
                }

            }

            optional<V> get(const K& key){
                LockAllTiers lock(*this);
                for(size_t i=0; i< tiers_.size(); i++){
                    auto val = tiers_[i]->get(key);
                    if(val.has_value()){
                        if(i == 0){
                            return val;
                        }
                        tiers_[i]->remove(key);
                        insertWithCascade(0, key, *val);
                        return val;
                    }
                }
                return nullopt;
            }

            void put(const K& key, const V& value){
                LockAllTiers lock(*this);
                for(size_t i=1; i<tiers_.size(); i++){
                    tiers_[i]->remove(key);

                }
                insertWithCascade(0, key, value);
            }

            bool contains(const K& key){
                LockAllTiers lock(*this);
                for(const auto& tier: tiers_){
                    if(tier->contains(key))return true;
                }
                return false;
            }
            void debugPrint(std::ostream& os) const {
            LockAllTiers lock(*this);
            os << "==== MultiTierCache ====\n";
            for (const auto& tier : tiers_) {
                tier->debugPrint(os);
            }
            os << "========================\n";
        }


};

int main(){
    using K = int;
    using V = string;

    vector<unique_ptr<ICacheTier<K, V>>> tiers;
    tiers.push_back(make_unique<LRUCacheTier<K, V>>(2, "MEM"));
    tiers.push_back(make_unique<LRUCacheTier<K, V>>(2, "SSD"));
    tiers.push_back(make_unique<LRUCacheTier<K, V>>(2, "HDD"));
    
    MultiTierCache<K, V> cache(move(tiers));
    cache.put(1, "A");
    cache.put(2, "B");
    cache.debugPrint(std::cout);
    cache.put(3, "C");
    cache.debugPrint(std::cout);

    auto v1 = cache.get(1);
    cout << "get (1) = " <<( v1 ? *v1: "MISS" )<<'\n';
    cache.debugPrint(std::cout);

    cache.put(4, "D"); // may evict something from MEM -> SSD -> HDD
    cache.put(5, "E");
    cache.put(6, "F");
    cache.debugPrint(std::cout);

    auto v99 = cache.get(99);
    std::cout << "get(99) = " << (v99 ? *v99 : "MISS") << "\n";

}