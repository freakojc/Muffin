#pragma once

#include <QtGlobal>

// Symbol visibility for the MuffinCore / MuffinUi shared libraries.
//
// The generated namespace .def exports project functions. Q_OBJECT classes
// additionally carry explicit class-level visibility for staticMetaObject and
// cross-boundary QObject::connect / QMetaObject references. Qt/STL helper
// instantiations are not part of the DLL interface.
// Under a STATIC MUFFIN_LIB_TYPE build (MUFFIN_SHARED_LIBS undefined) the
// macros expand to nothing.
#if !defined(MUFFIN_SHARED_LIBS)
#define MUFFIN_CORE_EXPORT
#define MUFFIN_UI_EXPORT
#elif defined(MUFFIN_CORE_LIBRARY)
#define MUFFIN_CORE_EXPORT Q_DECL_EXPORT
#define MUFFIN_UI_EXPORT Q_DECL_IMPORT
#elif defined(MUFFIN_UI_LIBRARY)
#define MUFFIN_CORE_EXPORT Q_DECL_IMPORT
#define MUFFIN_UI_EXPORT Q_DECL_EXPORT
#else
#define MUFFIN_CORE_EXPORT Q_DECL_IMPORT
#define MUFFIN_UI_EXPORT Q_DECL_IMPORT
#endif
