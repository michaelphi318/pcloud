CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

all: webmail_demo

webmail_demo: webmail.cc webmail.h webmail_demo.cc
	$(CXX) $(CXXFLAGS) webmail.cc webmail_demo.cc -o webmail_demo

clean:
	rm -f webmail_demo
