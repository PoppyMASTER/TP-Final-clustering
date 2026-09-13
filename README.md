# MiniHV — Hyperviseur minimal basé sur KVM

Prototype d'hyperviseur développé dans le cadre du TP final
(Parcours B — composant alternatif à QEMU).

## Présentation

MiniHV est un programme C qui communique directement avec
l'interface KVM du noyau Linux (`/dev/kvm`) pour créer,
configurer et exécuter des machines virtuelles minimales,
sans passer par QEMU ni libvirt.

Il remplace partiellement le rôle de QEMU en assurant :

- L'ouverture et le pilotage de KVM via des ioctls
- L'allocation et la gestion de la RAM guest
- La création et la configuration des vCPU
- L'interception des sorties I/O (port série)
- La boucle d'exécution et la gestion des VM Exit
- Les snapshots (sauvegarde/restauration de l'état RAM)

## Architecture

```
Utilisateur
    │
    ▼
minihv (CLI interactive)
    │  ioctls : KVM_CREATE_VM, KVM_CREATE_VCPU,
    │           KVM_SET_USER_MEMORY_REGION,
    │           KVM_SET_REGS, KVM_SET_SREGS, KVM_RUN
    ▼
/dev/kvm  ──  module KVM (noyau Linux)
    │
    ▼
CPU physique (Intel VT-x / AMD-V)
    │
    ▼
mini_os.bin  (programme assembleur 16 bits tournant dans la VM)
    │  out 0x3F8 (port série COM1)
    ▼
MiniHV intercepte KVM_EXIT_IO et affiche les caractères
```

## Composant remplacé

MiniHV remplace partiellement **QEMU** en reproduisant :

| Fonction QEMU           | MiniHV                          |
|-------------------------|---------------------------------|
| Ouvrir /dev/kvm         | `open("/dev/kvm", O_RDWR)`      |
| Créer une VM            | `ioctl(KVM_CREATE_VM)`          |
| Allouer la RAM          | `mmap` + `KVM_SET_USER_MEMORY_REGION` |
| Créer un vCPU           | `ioctl(KVM_CREATE_VCPU)`        |
| Configurer les registres| `KVM_SET_REGS` + `KVM_SET_SREGS`|
| Lancer le vCPU          | `ioctl(KVM_RUN)`                |
| Gérer les I/O           | `KVM_EXIT_IO` sur port 0x3F8   |
| Arrêter la VM           | `KVM_EXIT_HLT`                  |

Ce que MiniHV ne fait pas (hors périmètre) :
- Émulation de disque, réseau, GPU
- Mode protégé 32 bits / 64 bits complet
- BIOS/UEFI
- Migration de VM

## Prérequis

- Linux avec KVM activé
- Debian 12 ou Ubuntu 22.04+
- gcc, nasm, libreadline-dev
- Virtualisation imbriquée activée si VM hôte

```bash
sudo apt install gcc nasm libreadline-dev
```

Vérifier que KVM est disponible :

```bash
ls /dev/kvm
grep -E 'vmx|svm' /proc/cpuinfo | head -1
```

## Installation

```bash
git clone <repo>
cd kvm-proto
make
```

## Utilisation

### Mode direct

```bash
sudo ./minihv mini_os.bin
```

Lance directement `mini_os.bin` dans une VM nommée `vm0`.

### Mode CLI interactif

```bash
sudo ./minihv
```

Ouvre une CLI avec autocomplétion TAB et historique.

#### Commandes

| Commande                        | Description                    |
|---------------------------------|--------------------------------|
| `create <nom> <binaire>`        | Créer une VM                   |
| `start <nom>`                   | Démarrer une VM                |
| `list`                          | Lister les VMs et leur état    |
| `snapshot <nom> <fichier>`      | Sauvegarder l'état RAM         |
| `restore <nom> <fichier>`       | Restaurer un snapshot          |
| `destroy <nom>`                 | Supprimer une VM               |
| `help`                          | Afficher l'aide                |
| `quit`                          | Quitter                        |

#### Exemple de session

```
minihv> create vm0 mini_os.bin
[OK] VM 'vm0' créée — binaire 'mini_os.bin' (244 octets)

minihv> list
  NOM              ÉTAT         BINAIRE
  vm0              créée        mini_os.bin

minihv> start vm0
[INFO] Démarrage VM 'vm0'...
========================================
   MiniOS v2 - Running inside MiniHV
========================================
[OS]   Hello from KVM - tout fonctionne!
[OS]   Arret sur HLT.
[OK] VM 'vm0' — HLT

minihv> snapshot vm0 snap1.bin
[OK] Snapshot 'snap1.bin' créé pour VM 'vm0' (128 Mo)

minihv> destroy vm0
[OK] VM 'vm0' détruite
```

## Format des snapshots

Un snapshot est un fichier binaire structuré :

```
[Header 4B magic "MHIV"][ram_size 8B][vm_name 64B][timestamp 32B]
[RAM complète — 128 Mo]
```

Le magic `MHIV` (0x4D484956) permet de détecter les fichiers
corrompus ou invalides à la restauration.

## Journalisation

Toutes les actions sont enregistrées dans `kvm.log` :

```
[2026-09-11 11:38:07][OK] VM 'vm0' créée — binaire 'mini_os.bin'
[2026-09-11 11:38:22][INFO] Démarrage VM 'vm0'...
[2026-09-11 11:38:22][OK] VM 'vm0' — HLT
[2026-09-11 11:38:35][OK] VM 'vm0' détruite
```

Format : `[timestamp][niveau] message`
Niveaux : `INFO`, `OK`, `ERROR`, `WARN`

## Sécurité et validation

- Nom de VM : alphanumérique + `-` et `_` uniquement
- Binaire : vérifié existant et non vide avant chargement
- Snapshot : vérifié par magic number avant restauration
- VM existante : refus de création en doublon
- VM en cours : refus de destruction sans arrêt préalable
- Taille mémoire : vérifiée avant copie du binaire
- Maximum 8 VMs simultanées

## Limites

- Mode réel 16 bits uniquement (pas de mode protégé/64 bits)
- Pas d'émulation de disque ni de réseau
- Pas de BIOS — le binaire est chargé directement à 0x1000
- Les snapshots sauvegardent la RAM mais pas les registres vCPU
- Un seul vCPU par VM

## Améliorations possibles

- Mode protégé 32 bits avec GDT complète
- Chargement de format ELF
- Émulation d'un contrôleur disque simple (lecture de secteurs)
- Snapshot des registres vCPU
- Interface réseau via TAP
- Migration de VM entre deux instances MiniHV

## Structure du projet

```
kvm-proto/
├── minihv_j2.c      # Moteur KVM + CLI (source principal)
├── mini_os_v2.asm   # Mini OS guest en assembleur 16 bits
├── Makefile         # Compilation
├── kvm.log          # Journal des actions (généré à l'exécution)
└── README.md        # Ce fichier
```

## Comparaison avec QEMU

| Critère              | QEMU                    | MiniHV                  |
|----------------------|-------------------------|-------------------------|
| Lignes de code       | ~2 000 000              | ~400                    |
| Architectures CPU    | 20+                     | x86 16 bits uniquement  |
| Périphériques        | 200+                    | Port série COM1         |
| Formats disque       | RAW, QCOW2, VMDK…       | Binaire flat            |
| Interface            | CLI + QMP + libvirt     | CLI interactive         |
| Snapshots            | Oui (complets)          | RAM uniquement          |
| Performance          | Quasi-native avec KVM   | Quasi-native avec KVM   |
| Dépendances          | Nombreuses              | gcc + nasm + readline   |

La performance CPU est identique car les deux utilisent KVM
pour l'exécution des instructions — c'est le vrai CPU physique
qui exécute le code guest dans les deux cas.
