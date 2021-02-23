bin_PROGRAMS = feednix
feednix_configdir = $(sysconfdir)/xdg/feednix
dist_feednix_config_DATA = ../config.json

feednix_SOURCES = \
	CursesProvider.cpp \
	CursesProvider.h \
	FeedlyProvider.cpp \
	FeedlyProvider.h \
	main.cpp

feednix_CPPFLAGS = \
	-DDEBUG \
	-D'FEEDNIX_SYSTEM_CONFIG="$(feednix_configdir)/config.json"' \
	-I/usr/include/jsoncpp \
	-std=c++17 \
	-Wall \
	$(AM_CFLAGS)

AM_CFLAGS = -lcurl -ljsoncpp -lmenuw -lpanelw -lncursesw
AM_LIBS = curl jsoncpp menuw panelw ncursesw
