#if 1
#include "my_cache.h"
#include <stdlib.h>


int main(int argc, char **argv)
{
    int k = 5;
    lxh::cache::MyCache<int,int, lxh::cache::CacheType::FIFO> mc(k);
    //lxh::cache::MyCache<int,int, lxh::cache::CacheType::LRU> mc(k);
    mc.with_expire_time(10).Start();

    for(int i = 0; i < 5; i++) {
        printf("--------------------------------------------\n");
        mc.Put(i,i);
        mc.Debug();
        //std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    for(int i = 0; i < 20; i++) {
        printf("################################################################################n");
        int key = rand() % 7;
        printf("key: %d\n", key);
        auto res = mc.Get(key);
        mc.Debug();
        if(res.second) {
            std::cout << "hit" << std::endl;
        } else {
            std::cout << "miss" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

#else
#include "t.h"
int main(int argc, char ** argv)
{
    LRUCache<int,int> aa(10,10,true);
    return 0;
}
#endif
