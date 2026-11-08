TARGETS = kvstore test coordinator

all: $(TARGETS)

kvstore: kvStorage.o
	g++ -std=c++17 -pthread $^ -o $@

coordinator: backendCoordinator.o
	g++ -std=c++17 -pthread $^ -o $@

test: testKVStorage.o
	g++ -std=c++17 -pthread $^ -o $@

clean::
	rm -fv $(TARGETS) *~ *.o
	rm -rfv data_*
