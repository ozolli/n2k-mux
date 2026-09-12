/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * inimerge.h — Fusion d'un INI régénéré dans un INI existant, en préservant
 * commentaires, ordre et mise en page.
 *
 * L'interface web régénère l'INI ENTIER depuis les structures parsées : sans ce
 * module, un seul « Enregistrer » effaçait tous les commentaires du fichier de
 * configuration. Ici on prend l'ANCIEN fichier comme squelette et on n'y touche
 * que ce qui change :
 *
 *   - une ligne « clé = valeur » dont la clé existe dans le nouveau contenu est
 *     réécrite avec la nouvelle valeur, en gardant l'orthographe de la clé,
 *     l'alignement du '=' et le commentaire de fin de ligne ;
 *   - une clé absente du nouveau contenu est supprimée (l'éditeur l'a retirée) ;
 *   - une clé nouvelle est ajoutée à la fin de sa section ;
 *   - une section entièrement nouvelle est ajoutée à la fin du fichier ;
 *   - tout le reste (commentaires, lignes vides, en-têtes de section) est recopié
 *     tel quel.
 *
 * Les alias de section reconnus par le parser (priority/priorities/priorites,
 * rate/rates, talker/talkers, sentence/sentences) sont appariés entre eux.
 *
 * Comme le reste du projet : zéro allocation dynamique, tampons fournis par
 * l'appelant, tolérant (une ligne incomprise est recopiée, pas rejetée).
 */

#ifndef N2KMUX_INIMERGE_H
#define N2KMUX_INIMERGE_H

#include <stddef.h>

#define INIM_MAX_ENTRIES  256   /* lignes « clé = valeur » du nouveau contenu */
#define INIM_MAX_SECTIONS  24
#define INIM_SEC_LEN       32
#define INIM_KEY_LEN       64
#define INIM_VAL_LEN      256

/*
 * Écrit dans `out` le résultat de la fusion de `new_text` dans `old_text`.
 * Retourne la longueur écrite, ou -1 si le tampon est trop petit ou si le
 * nouveau contenu dépasse les limites ci-dessus. En cas d'échec, l'appelant
 * doit se rabattre sur `new_text` tel quel (ne jamais perdre l'édition).
 */
int ini_merge(const char *old_text, const char *new_text,
              char *out, size_t outsz);

#endif /* N2KMUX_INIMERGE_H */
