#-------------------------------------------------
#
# Project created by QtCreator 2015-09-14T08:06:16
#
#-------------------------------------------------

include(../mbase/mbase.pri)

QT += concurrent testlib
QT -= gui

TARGET = qwamp
TEMPLATE = lib

CONFIG += c++11

QMAKE_CXXFLAGS += -pthread
QMAKE_CXXFLAGS_DEBUG += -DDEBUG

INCLUDEPATH += ../qmsgpack/src

SOURCES += \
    qwamp.cpp \
    crossbarcomponent.cpp

HEADERS +=\
    qwamp.h \
    crossbarcomponent.h

DISTFILES += \
    qwamp.pri
