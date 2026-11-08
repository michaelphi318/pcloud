TARGETS = frontend_server
all: $(TARGETS)

frontend_server: frontend.cc
	g++ frontend.cc -o frontend_server -lpthread