TARGET=Test
LIBS=-lpthread
CC=g++
#CFLAGS=-std=c++17 -g -Wall
CFLAGS=-std=c++17 -Wall -O3 -I /apps/tools/cent_os72/thirdparty/boost/boost_1_64_0/include/

.PHONY: default all clean

default: $(TARGET)
all: default

OBJECTS=$(patsubst %.cpp, %.o, $(wildcard *.cpp))
HEADERS=$(wildcard *.h) Makefile

%.o: %.cpp $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -Wall $(LIBS) -o $@

clean:
	-rm -f *.o
	-rm -f $(TARGET)
