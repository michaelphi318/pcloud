CXXFLAGS = -std=c++17 -Wall -I. -Imodules/utilities -Imodules/storage-client -Imodules/route-handlers -I../backend/webmail

SRCS = 	frontend.cc \
		modules/utilities/utils.cc \
		modules/storage-client/kvClient.cc \
		modules/route-handlers/handlers.cc \
		../backend/webmail/webmail.cc

TARGET = frontend_server

all: $(TARGET)

$(TARGET): $(SRCS)
	g++ $(CXXFLAGS) $(SRCS) -o $(TARGET) -lpthread