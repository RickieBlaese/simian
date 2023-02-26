clang++ src/monkey.cc /home/stole/color/color.cc -o monkey -L/home/stole/fmt/ -lfmt -lmenu $(ncursesw5-config --cflags --libs) -lssl -lcrypto -lrapidfuzz -std=c++20 -g
