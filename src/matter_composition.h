#pragma once
#include <stdint.h>

// ===========================================================================
//  Composition des endpoints et ConfigurationVersion (Basic Information, EP0)
//
//  Terrain du 06/10 : depuis la 0.3.0, EP4 « Halo auto » n'est plus expose,
//  mais Apple Home garde sa tuile, « Sans reponse », meme apres un redemarrage
//  du pont et du concentrateur. Le noeud n'a jamais dit que sa composition
//  avait change : ConfigurationVersion (Matter 1.4) sert a cela, un controleur
//  relit l'appareil quand elle monte.
//
//  La composition resume ce qui change la liste des endpoints (EP4 expose ou
//  non, EP2 et EP3 en lampes ou en prises), avec une revision a monter a la
//  main si une autre difference d'endpoints apparait. Le pont la garde en NVS
//  avec la version ; au demarrage, next() dit la version a publier et ce qu'il
//  faut ecrire. Aucune composition gardee (premier demarrage d'un firmware qui
//  la suit) compte comme un changement : EP4 a pu disparaitre sans que le
//  noeud le dise. La version ne redescend jamais, et vaut au moins 1.
//
//  Pur et sans Arduino : teste sur l'hote (tools/host_tests).
// ===========================================================================

struct MatterComposition {
  static constexpr uint32_t kRevision = 1;  // a monter si la liste des endpoints change autrement

  static constexpr uint32_t composition(bool exposeAuto, bool selectorsAsLights) {
    return (kRevision << 8) | (exposeAuto ? 1u : 0u) | (selectorsAsLights ? 2u : 0u);
  }

  struct Step {
    uint32_t version;       // version a publier
    bool storeVersion;      // l'ecrire en NVS (elle a monte)
    bool storeComposition;  // ecrire la composition actuelle en NVS
  };

  static Step next(bool versionKnown, uint32_t storedVersion, bool compositionKnown, uint32_t storedComposition,
                   uint32_t current) {
    const uint32_t base = versionKnown && storedVersion >= 1 ? storedVersion : 1;
    const bool changed = !versionKnown || !compositionKnown || storedComposition != current;
    if (!changed) return {base, false, false};
    if (base == UINT32_MAX) return {base, false, true};
    return {base + 1, true, true};
  }
};
