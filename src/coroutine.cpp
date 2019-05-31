#include "../include/private/coroutine_p.h"

QTNETWORKNG_NAMESPACE_BEGIN

CoroutineException::CoroutineException()
{
}


CoroutineException::CoroutineException(CoroutineException &)
{
}


CoroutineException::~CoroutineException()
{}


void CoroutineException::raise()
{
    throw *this;
}


QString CoroutineException::what() const
{
    return QStringLiteral("coroutine base exception.");
}


CoroutineException *CoroutineException::clone() const
{
    return new CoroutineException();
}


CoroutineExitException::CoroutineExitException()
{
}


void CoroutineExitException::raise()
{
    throw *this;
}


QString CoroutineExitException::what() const
{
    return QStringLiteral("coroutine was asked to quit.");
}


CoroutineException *CoroutineExitException::clone() const
{
    return new CoroutineExitException();
}


CoroutineInterruptedException::CoroutineInterruptedException()
{
}


void CoroutineInterruptedException::raise()
{
    throw *this;
}


QString CoroutineInterruptedException::what() const
{
    return QStringLiteral("coroutine was interrupted.");
}


CoroutineException *CoroutineInterruptedException::clone() const
{
    return new CoroutineInterruptedException();
}


quintptr BaseCoroutine::id() const
{
    const BaseCoroutine *p = this;
    return reinterpret_cast<quintptr>(p);
}


void BaseCoroutine::run()
{

}


CurrentCoroutineStorage &currentCoroutine()
{
    static CurrentCoroutineStorage storage;
    return storage;
}


BaseCoroutine *CurrentCoroutineStorage::get()
{
    if(storage.hasLocalData())
    {
        return storage.localData().value;
    }
    BaseCoroutine *main = createMainCoroutine();
    main->setObjectName("main_coroutine");
    storage.localData().value = main;
    return main;
}


void CurrentCoroutineStorage::set(BaseCoroutine *coroutine)
{
    storage.localData().value = coroutine;
}


void CurrentCoroutineStorage::clean()
{
    if(storage.hasLocalData())
    {
        storage.localData().value = nullptr;
    }
}


BaseCoroutine *BaseCoroutine::current()
{
    return currentCoroutine().get();
}


QDebug &operator <<(QDebug &out, const BaseCoroutine& coroutine)
{
    if(coroutine.objectName().isEmpty()) {
        return out << QString::fromLatin1("BaseCourtine(id=%1)").arg(coroutine.id());
    } else {
        return out << QString::fromLatin1("%1(id=%2)").arg(coroutine.objectName()).arg(coroutine.id());
    }
}


QTNETWORKNG_NAMESPACE_END
