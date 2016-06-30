#-------------------------------------------------
#
# Project created by QtCreator 2015-09-14T08:06:16
#
#-------------------------------------------------


QT += concurrent testlib
QT -= gui

TARGET = qwamp
TEMPLATE = lib

CONFIG += c++11

QMAKE_CXXFLAGS += -pthread
QMAKE_CXXFLAGS_DEBUG += -DDEBUG

INCLUDEPATH += ../qmsgpack/src

SOURCES += \
    qwamp.cpp

HEADERS +=\
    qwamp.h

DISTFILES += \
    qwamp.pri \
    README.md
