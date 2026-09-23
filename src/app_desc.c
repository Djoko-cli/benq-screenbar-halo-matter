// ===========================================================================
//  Descripteur d'application (esp_app_desc_t) propre au projet
//
//  Les bibliotheques precompilees du core Arduino portent le descripteur du
//  lib-builder : version "6671d0b" (son commit), nom de projet "arduino-lib-
//  builder". Or la pile Matter rapporte esp_app_get_description()->version
//  comme SoftwareVersionString : c'est le "Programme interne" d'Apple Home.
//
//  Celui de libesp_app_format.a (esp_app_desc.c d'IDF 5.5) est defini en
//  symbole FAIBLE : cette definition forte le remplace a l'edition des liens,
//  et sa section .rodata_desc, plus referencee, disparait (--gc-sections).
//  Meme contenu que celui d'IDF, sauf version et nom de projet. Le condense
//  app_elf_sha256 reste a zero : esptool l'ecrit dans l'image (elf2image).
//
//  Controle apres compilation : nm firmware.elf montre esp_app_desc en R (et
//  non V), firmware.map place .rodata_desc de app_desc.c.o en tete de
//  .flash.appdesc, et 'firmware' au demarrage n'a pas d'alerte.
// ===========================================================================

#include <esp_app_desc.h>
#include <sdkconfig.h>

#include "fw_version.h"

#ifndef IDF_VER
#error "IDF_VER manquant : attendu du core Arduino-ESP32 (drapeaux de compilation)"
#endif

const __attribute__((section(".rodata_desc"), used)) esp_app_desc_t esp_app_desc = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
#ifdef CONFIG_BOOTLOADER_APP_SECURE_VERSION
    .secure_version = CONFIG_BOOTLOADER_APP_SECURE_VERSION,
#else
    .secure_version = 0,
#endif
    .version = FW_VERSION_FULL,
    .project_name = "benq-halo",
#ifdef CONFIG_APP_COMPILE_TIME_DATE
    .time = __TIME__,
    .date = __DATE__,
#else
    .time = "",
    .date = "",
#endif
    .idf_ver = IDF_VER,
    .min_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL,
    .max_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL,
    .mmu_page_size = 31 - __builtin_clz(CONFIG_MMU_PAGE_SIZE),
};

// Zero final compris : 31 caracteres au plus, comme IDF le verifie pour
// PROJECT_VER. FW_VERSION + "-" + hash (12 au plus) + "-dirty" tient en 25.
_Static_assert(sizeof(FW_VERSION_FULL) <= sizeof(esp_app_desc.version),
               "FW_VERSION-FW_GIT_REV trop long pour esp_app_desc.version (31 caracteres)");
_Static_assert(sizeof(IDF_VER) <= sizeof(esp_app_desc.idf_ver), "IDF_VER trop long pour esp_app_desc.idf_ver");
