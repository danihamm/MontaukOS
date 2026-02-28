/*
    * main.cpp
    * Hello world program for MontaukOS
    * Copyright (c) 2025 Daniel Hammer
*/

#include <montauk/syscall.h>

extern "C" void _start() {


    montauk::print("Hello from userspace!\n");

    // while(true) {
    //     montauk::print("ab");
    // }

}
