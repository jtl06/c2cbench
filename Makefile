CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
c2cbench: c2cbench.cpp harness.hpp cas.hpp chase.hpp
	$(CXX) $(CXXFLAGS) -pthread $< -o $@
clean:
	rm -f c2cbench
