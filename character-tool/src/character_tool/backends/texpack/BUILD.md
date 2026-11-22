# Сборка texpack

Для сборки нужны компилятор C++17, заголовок `stb_image.h` и libsquish. Укажите в `STB_INCLUDE` и `SQUISH_INCLUDE` каталоги с `stb_image.h` и `squish.h`, а в `SQUISH_LIB` — полный путь к соответствующей библиотеке, например `libsquish.a`. Из каталога с `texpack.cpp` выполните

```sh
c++ -std=c++17 -O2 -pthread -I"$STB_INCLUDE" -I"$SQUISH_INCLUDE" \
  texpack.cpp "$SQUISH_LIB" -o texpack
```

Эта команда подходит для статической libsquish, собранной с `-DBUILD_SQUISH_WITH_OPENMP=OFF`. Если libsquish собрана GCC с OpenMP, добавьте `-fopenmp` к команде линковки.

Готовый бинарник также можно передать инструменту напрямую. Упаковщик принимает каталоги входных PNG и выходных DDS в таком порядке.

```sh
./texpack input-png-dir output-dds-dir
```
