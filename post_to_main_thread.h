#ifndef _POST_TO_MAIN_THREAD_H
#define _POST_TO_MAIN_THREAD_H 1

#include <QApplication>
#include <QObject>
#include <memory>

// http://stackoverflow.com/questions/21646467/how-to-execute-a-functor-in-a-given-thread-in-qt-gcd-style
template<typename F>
static inline void post_to_main_thread(F &&fun)
{
	QObject signalSource;
	QObject::connect(&signalSource, &QObject::destroyed, qApp, std::move(fun));
}

#endif  // !defined(_POST_TO_MAIN_THREAD_H)
