all:
	@make clean
	cnet -J randomwalk.c
	g++ -shared -fPIC -std=c++11 -I../cnet-3.3.3/src -c project.cpp

run:
	@make all
	cnet PROJECT

clean:
	rm -rf f? *.o *.cnet result.*
