#ifndef PIL_ClassLoader_INCLUDED
#define PIL_ClassLoader_INCLUDED


#include "../Environment.h"
#include "MetaObject.h"
#include "Manifest.h"
#include "SharedLibrary.h"
#include "../Thread/Mutex.h"
#include "../Debug/Exception.h"
#include <map>
#include <vector>
#include <sstream>

namespace pi {


template <class Base>
class ClassLoader
    /// The ClassLoader loads C++ classes from shared libraries
    /// at runtime. It must be instantiated with a root class
    /// of the loadable classes.
    /// For a class to be loadable from a library, the library
    /// must provide a Manifest of all the classes it contains.
    /// The Manifest for a shared library can be easily built
    /// with the help of the macros in the header file
    /// "ClassLibrary.h".
    ///
    /// A class library can
    /// export multiple manifests. In addition to the default
    /// (unnamed) manifest, multiple named manifests can
    /// be exported, each having a different base class.
    ///
    /// There is one important restriction: one instance of
    /// ClassLoader can only load one manifest from a class
    /// library.
{
public:
    typedef AbstractMetaObject<Base> Meta;
    typedef Manifest<Base> Manif;
    typedef void (*InitializeLibraryFunc)();
    typedef void (*UninitializeLibraryFunc)();
    typedef bool (*BuildManifestFunc)(ManifestBase*);

    struct LibraryInfo
    {
        SharedLibrary* pLibrary;
        const Manif*   pManifest;
        int            refCount;
    };
    typedef std::map<std::string, LibraryInfo> LibraryMap;

    class Iterator
        /// The ClassLoader's very own iterator class.
    {
    public:
        typedef std::pair<std::string, const Manif*> Pair;

        Iterator(const typename LibraryMap::const_iterator& it)
        {
            _it = it;
        }
        Iterator(const Iterator& it)
        {
            _it = it._it;
        }
        ~Iterator()
        {
        }
        Iterator& operator = (const Iterator& it)
        {
            _it = it._it;
            return *this;
        }
        inline bool operator == (const Iterator& it) const
        {
            return _it == it._it;
        }
        inline bool operator != (const Iterator& it) const
        {
            return _it != it._it;
        }
        Iterator& operator ++ () // prefix
        {
            ++_it;
            return *this;
        }
        Iterator operator ++ (int) // postfix
        {
            Iterator result(_it);
            ++_it;
            return result;
        }
        inline const Pair* operator * () const
        {
            _pair.first  = _it->first;
            _pair.second = _it->second.pManifest;
            return &_pair;
        }
        inline const Pair* operator -> () const
        {
            _pair.first  = _it->first;
            _pair.second = _it->second.pManifest;
            return &_pair;
        }

    private:
        typename LibraryMap::const_iterator _it;
        mutable Pair _pair;
    };

    ClassLoader()
        /// Creates the ClassLoader.
    {
    }

    virtual ~ClassLoader()
        /// Destroys the ClassLoader.
    {
        unloadLibrary();
        for (typename LibraryMap::const_iterator it = _map.begin(); it != _map.end(); ++it)
        {
            delete it->second.pLibrary;
            delete it->second.pManifest;
        }
    }

    void loadLibrary(const std::string& path, const std::string& manifest)
        /// Loads a library from the given path, using the given manifest.
        /// Does nothing if the library is already loaded.
        /// Throws a LibraryLoadException if the library
        /// cannot be loaded or does not have a Manifest.
        /// If the library exports a function named "PILInitializeLibrary",
        /// this function is executed.
        /// If called multiple times for the same library,
        /// the number of calls to unloadLibrary() must be the same
        /// for the library to become unloaded.
    {
        FastMutex::ScopedLock lock(_mutex);

        typename LibraryMap::iterator it = _map.find(path);
        if (it == _map.end())
        {
            LibraryInfo li;
            li.pLibrary  = new SharedLibrary(path);
            li.pManifest = new Manif();
            li.refCount  = 1;
            try
            {
                std::string PILBuildManifestSymbol("PILBuildManifest");
                PILBuildManifestSymbol.append(manifest);
                if (li.pLibrary->hasSymbol("PILInitializeLibrary"))
                {
                    InitializeLibraryFunc initializeLibrary = (InitializeLibraryFunc) li.pLibrary->getSymbol("PILInitializeLibrary");
                    initializeLibrary();
                }
                if (li.pLibrary->hasSymbol(PILBuildManifestSymbol))
                {
                    BuildManifestFunc buildManifest = (BuildManifestFunc) li.pLibrary->getSymbol(PILBuildManifestSymbol);
                    if (buildManifest(const_cast<Manif*>(li.pManifest)))
                        _map[path] = li;
                    else
                        throw LibraryLoadException(std::string("Manifest class mismatch in ") + path, manifest);
                }
                else throw LibraryLoadException(std::string("No manifest in ") + path, manifest);
            }
            catch (...)
            {
                delete li.pLibrary;
                delete li.pManifest;
                throw;
            }
        }
        else
        {
            ++it->second.refCount;
        }
    }

    void loadLibrary(const std::string& path)
        /// Loads a library from the given path. Does nothing
        /// if the library is already loaded.
        /// Throws a LibraryLoadException if the library
        /// cannot be loaded or does not have a Manifest.
        /// If the library exports a function named "PILInitializeLibrary",
        /// this function is executed.
        /// If called multiple times for the same library,
        /// the number of calls to unloadLibrary() must be the same
        /// for the library to become unloaded.
        ///
        /// Equivalent to loadLibrary(path, "").
    {
        loadLibrary(path, "");
    }

    void unloadLibrary(const std::string& path="")
        /// Unloads the given library.
        /// Be extremely cautious when unloading shared libraries.
        /// If objects from the library are still referenced somewhere,
        /// a total crash is very likely.
        /// If the library exports a function named "PILUninitializeLibrary",
        /// this function is executed before it is unloaded.
        /// If loadLibrary() has been called multiple times for the same
        /// library, the number of calls to unloadLibrary() must be the same
        /// for the library to become unloaded.
    {
        if(path.empty())
        {
            std::vector<std::string> libs2unload;
            {
                FastMutex::ScopedLock lock(_mutex);

                for (typename LibraryMap::const_iterator it = _map.begin(); it != _map.end(); ++it)
                {
                    libs2unload.push_back(it->second.pLibrary->getPath());
                }
            }
            for(const std::string& libPath:libs2unload) unloadLibrary(libPath);
        }
        else
        {
        FastMutex::ScopedLock lock(_mutex);

        typename LibraryMap::iterator it = _map.find(path);
        if (it != _map.end())
        {
            if (--it->second.refCount == 0)
            {
                if (it->second.pLibrary->hasSymbol("PILUninitializeLibrary"))
                {
                    UninitializeLibraryFunc uninitializeLibrary = (UninitializeLibraryFunc) it->second.pLibrary->getSymbol("PILUninitializeLibrary");
                    uninitializeLibrary();
                }
                delete it->second.pManifest;
                it->second.pLibrary->unload();
                delete it->second.pLibrary;
                _map.erase(it);
            }
        }
        else throw NotFoundException(path);
        }
    }

    const Meta* findClass(const std::string& className) const
        /// Returns a pointer to the MetaObject for the given
        /// class, or a null pointer if the class is not known.
    {
        FastMutex::ScopedLock lock(_mutex);

        for (typename LibraryMap::const_iterator it = _map.begin(); it != _map.end(); ++it)
        {
            const Manif* pManif = it->second.pManifest;
            typename Manif::Iterator itm = pManif->find(className);
            if (itm != pManif->end())
                return *itm;
        }
        return 0;
    }

    std::string info()const
    {
        std::stringstream os;
        FastMutex::ScopedLock lock(_mutex);

        for (typename LibraryMap::const_iterator it = _map.begin(); it != _map.end(); ++it)
        {
            const Manif* pManif = it->second.pManifest;
            os<<"Plugin "<<it->second.pLibrary->getPath()<<":\n";
            for(typename Manif::Iterator itm=pManif->begin();itm!=pManif->end();itm++)
            {
                os<<"  "<<itm.first()<<std::endl;
            }
        }
        return os.str();
    }

    std::vector<std::string> names()const
    {
        std::vector<std::string> results;
        FastMutex::ScopedLock lock(_mutex);

        for (typename LibraryMap::const_iterator it = _map.begin(); it != _map.end(); ++it)
        {
            const Manif* pManif = it->second.pManifest;
            for(typename Manif::Iterator itm=pManif->begin();itm!=pManif->end();itm++)
            {
                results.push_back(itm.first());
            }
        }
        return results;
    }

    friend std::ostream& operator<<(std::ostream& os,const ClassLoader& rhs)
    {
        os<<rhs.info();
        return os;
    }

    const Meta& classFor(const std::string& className) const
        /// Returns a reference to the MetaObject for the given
        /// class. Throws a NotFoundException if the class
        /// is not known.
    {
        const Meta* pMeta = findClass(className);
        if (pMeta)
            return *pMeta;
        else
            throw NotFoundException(className);
    }

    Base* create(const std::string& className) const
        /// Creates an instance of the given class.
        /// Throws a NotFoundException if the class
        /// is not known.
    {
        return classFor(className).create();
    }

    Base& instance(const std::string& className) const
        /// Returns a reference to the sole instance of
        /// the given class. The class must be a singleton,
        /// otherwise an InvalidAccessException will be thrown.
        /// Throws a NotFoundException if the class
        /// is not known.
    {
        return classFor(className).instance();
    }

    bool canCreate(const std::string& className) const
        /// Returns true if create() can create new instances
        /// of the class.
    {
        return classFor(className).canCreate();
    }

    void destroy(const std::string& className, Base* pObject) const
        /// Destroys the object pObject points to.
        /// Does nothing if object is not found.
    {
        classFor(className).destroy(pObject);
    }

    bool isAutoDelete(const std::string& className, Base* pObject) const
        /// Returns true if the object is automatically
        /// deleted by its meta object.
    {
        return classFor(className).isAutoDelete(pObject);
    }

    const Manif* findManifest(const std::string& path) const
        /// Returns a pointer to the Manifest for the given
        /// library, or a null pointer if the library has not been loaded.
    {
        FastMutex::ScopedLock lock(_mutex);

        typename LibraryMap::const_iterator it = _map.find(path);
        if (it != _map.end())
            return it->second.pManifest;
        else
            return 0;
    }

    const Manif& manifestFor(const std::string& path) const
        /// Returns a reference to the Manifest for the given library
        /// Throws a NotFoundException if the library has not been loaded.
    {
        const Manif* pManif = findManifest(path);
        if (pManif)
            return *pManif;
        else
            throw NotFoundException(path);
    }

    bool isLibraryLoaded(const std::string& path) const
        /// Returns true if the library with the given name
        /// has already been loaded.
    {
        return findManifest(path) != 0;
    }

    Iterator begin() const
    {
        FastMutex::ScopedLock lock(_mutex);

        return Iterator(_map.begin());
    }

    Iterator end() const
    {
        FastMutex::ScopedLock lock(_mutex);

        return Iterator(_map.end());
    }

private:
    LibraryMap _map;
    mutable FastMutex _mutex;
};


} // namespace PIL


#endif // PIL_ClassLoader_INCLUDED
