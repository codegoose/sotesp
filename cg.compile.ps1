"Compiling 'cg.cpp' -> 'cg.o' ..."
g++ -I..\include -L..\library -DCURL_STATICLIB -c cg.cpp --std=c++17 -o cg.o -O3