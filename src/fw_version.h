#pragma once

// ===========================================================================
//  Version du firmware (C et C++ : app_desc.c l'inclut aussi)
//
//  FW_VERSION, FW_GIT_REV et FW_ENV viennent de platformio.ini
//  (build_src_flags, donc pour src/ seulement) : la version a la main, la
//  revision par tools/git_rev.py, le nom de l'env PlatformIO par $PIOENV. Les
//  valeurs ci-dessous ne servent qu'a une compilation hors PlatformIO.
//
//  0.4.0 : protocole JSON de l'app compagnon (docs/PROTOCOLE-JSON.md, 'json').
//  L'app exige 0.4.0 au moins.
//
//  FW_VERSION_FULL ("0.4.0-903414e", "-dirty" si l'arbre differe du commit)
//  est la version affichee au demarrage et par 'info', et celle du descripteur
//  d'application (esp_app_desc.version, src/app_desc.c) : Matter la rapporte
//  comme SoftwareVersionString, le "Programme interne" d'Apple Home.
// ===========================================================================

#ifndef FW_VERSION
#define FW_VERSION "0.4.0"
#endif
#ifndef FW_GIT_REV
#define FW_GIT_REV "nogit"
#endif
#ifndef FW_ENV
#define FW_ENV "inconnu"  // env PlatformIO ("esp32c6thread"...), rapporte par hello.env
#endif
#define FW_VERSION_FULL FW_VERSION "-" FW_GIT_REV
