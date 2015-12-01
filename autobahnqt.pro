#-------------------------------------------------
#
# Project created by QtCreator 2015-09-14T08:06:16
#
#-------------------------------------------------

include(../mbase/mbase.pri)

QT       -= gui

TARGET = autobahnqt
TEMPLATE = lib

CONFIG += c++11

QMAKE_CXXFLAGS += -Wno-unused-parameter   #kvuli msgpack
QMAKE_CXXFLAGS += -pthread
QMAKE_CXXFLAGS_DEBUG += -DDEBUG

INCLUDEPATH += ../msgpack-c/include

SOURCES += \
    autobahn_qt.cpp \
    crossbarservice.cpp

HEADERS +=\
    autobahn_qt.h \
    crossbarservice.h

DISTFILES += \
    autobahnqt.pri
