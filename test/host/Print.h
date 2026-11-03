#pragma once
#include <Arduino.h>
class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t)=0;
    virtual size_t write(const uint8_t*,size_t)=0;
    size_t println(const char* s) { return write((const uint8_t*)s,strlen(s)); }
    template<class... T> size_t printf(const char* fmt,T... args) {
        char b[512]; int n=snprintf(b,sizeof(b),fmt,args...);
        return write((const uint8_t*)b,min((size_t)max(n,0),sizeof(b)-1));
    }
};
