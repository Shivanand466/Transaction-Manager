#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#define ORDER 4
#define MAX_KEYS (ORDER - 1)

#define TX_FILE "transactions.txt"
#define SELLER_FILE "sellers.txt"

#define RATE_LIMIT 300.0
#define MAX_AMOUNT 1e9
#define REGULAR_MIN 6   /* "more than 5 transactions" */

typedef struct Transaction {
    int id;
    int buyer_id;
    int seller_id;
    double energy;
    double price;
    double total;
    time_t timestamp;
} Transaction;

typedef struct Pair {
    int seller_id;
    int buyer_id;
    int count;
} Pair;

typedef struct RegularBuyer {
    int buyer_id;
    Pair *pair;
    struct RegularBuyer *next;
} RegularBuyer;


/* Transaction tree, keyed by transaction id.
   Used for the main record and for every seller/buyer subtree. */
typedef struct TxNode {
    int n;
    bool leaf;
    int keys[MAX_KEYS + 1];
    Transaction *rec[MAX_KEYS + 1];
    struct TxNode *child[ORDER + 1];
    struct TxNode *next;
} TxNode;

typedef struct TxTree {
    TxNode *root;
    int count;
} TxTree;

typedef struct Seller {
    int id;
    double rate_low;    /* up to 300 kWh */
    double rate_high;   /* above 300 kWh */
    RegularBuyer *regulars;
    TxTree txns;
} Seller;

typedef struct Buyer {
    int id;
    double total_energy;
    TxTree txns;
} Buyer;

/* Seller tree, keyed by seller id */
typedef struct SellerNode {
    int n;
    bool leaf;
    int keys[MAX_KEYS + 1];
    Seller *rec[MAX_KEYS + 1];
    struct SellerNode *child[ORDER + 1];
    struct SellerNode *next;
} SellerNode;

typedef struct SellerTree {
    SellerNode *root;
    int count;
} SellerTree;

/* Buyer tree, keyed by buyer id */
typedef struct BuyerNode {
    int n;
    bool leaf;
    int keys[MAX_KEYS + 1];
    Buyer *rec[MAX_KEYS + 1];
    struct BuyerNode *child[ORDER + 1];
    struct BuyerNode *next;
} BuyerNode;

typedef struct BuyerTree {
    BuyerNode *root;
    int count;
} BuyerTree;

/* Seller/buyer pair tree, keyed by (seller id, buyer id) */
typedef struct PairKey {
    int seller_id;
    int buyer_id;
} PairKey;

typedef struct PairNode {
    int n;
    bool leaf;
    PairKey keys[MAX_KEYS + 1];
    Pair *rec[MAX_KEYS + 1];
    struct PairNode *child[ORDER + 1];
    struct PairNode *next;
} PairNode;

typedef struct PairTree {
    PairNode *root;
    int count;
} PairTree;

/* Time index, keyed by (timestamp, transaction id) */
typedef struct TimeKey {
    time_t ts;
    int id;
} TimeKey;

typedef struct TimeNode {
    int n;
    bool leaf;
    TimeKey keys[MAX_KEYS + 1];
    Transaction *rec[MAX_KEYS + 1];
    struct TimeNode *child[ORDER + 1];
    struct TimeNode *next;
} TimeNode;

typedef struct TimeTree {
    TimeNode *root;
    int count;
} TimeTree;

/* Energy index, keyed by (energy, transaction id) */
typedef struct EnergyKey {
    double energy;
    int id;
} EnergyKey;

typedef struct EnergyNode {
    int n;
    bool leaf;
    EnergyKey keys[MAX_KEYS + 1];
    Transaction *rec[MAX_KEYS + 1];
    struct EnergyNode *child[ORDER + 1];
    struct EnergyNode *next;
} EnergyNode;

typedef struct EnergyTree {
    EnergyNode *root;
    int count;
} EnergyTree;


static TxTree allTxns;
static TimeTree timeIndex;
static EnergyTree energyIndex;
static SellerTree sellers;
static BuyerTree buyers;
static PairTree pairs;
static int nextTxId = 1;


static void *allocOrDie(size_t size)
{
    void *p = calloc(1, size);
    if (!p) {
        printf("Out of memory\n");
        exit(1);
    }
    return p;
}


/* ================= Transaction tree ================= */

static TxNode *txNewNode(bool leaf)
{
    TxNode *node = allocOrDie(sizeof(TxNode));
    node->leaf = leaf;
    return node;
}

static TxNode *txInsertRec(TxNode *node, Transaction *t, int *up)
{
    int i, j, mid;
    TxNode *right, *split;

    if (node->leaf) {
        for (i = node->n; i > 0 && node->keys[i - 1] > t->id; i--) {
            node->keys[i] = node->keys[i - 1];
            node->rec[i] = node->rec[i - 1];
        }
        node->keys[i] = t->id;
        node->rec[i] = t;
        node->n++;
        if (node->n <= MAX_KEYS)
            return NULL;

        mid = node->n / 2;
        right = txNewNode(true);
        for (j = mid; j < node->n; j++) {
            right->keys[j - mid] = node->keys[j];
            right->rec[j - mid] = node->rec[j];
        }
        right->n = node->n - mid;
        node->n = mid;
        right->next = node->next;
        node->next = right;
        *up = right->keys[0];
        return right;
    }

    for (i = 0; i < node->n && t->id >= node->keys[i]; i++)
        ;
    split = txInsertRec(node->child[i], t, up);
    if (!split)
        return NULL;

    for (j = node->n; j > i; j--) {
        node->keys[j] = node->keys[j - 1];
        node->child[j + 1] = node->child[j];
    }
    node->keys[i] = *up;
    node->child[i + 1] = split;
    node->n++;
    if (node->n <= MAX_KEYS)
        return NULL;

    mid = node->n / 2;
    right = txNewNode(false);
    *up = node->keys[mid];
    for (j = mid + 1; j < node->n; j++)
        right->keys[j - mid - 1] = node->keys[j];
    for (j = mid + 1; j <= node->n; j++)
        right->child[j - mid - 1] = node->child[j];
    right->n = node->n - mid - 1;
    node->n = mid;
    return right;
}

static void txInsert(TxTree *tree, Transaction *t)
{
    int up;
    TxNode *split, *root;

    if (!tree->root)
        tree->root = txNewNode(true);
    split = txInsertRec(tree->root, t, &up);
    if (split) {
        root = txNewNode(false);
        root->keys[0] = up;
        root->child[0] = tree->root;
        root->child[1] = split;
        root->n = 1;
        tree->root = root;
    }
    tree->count++;
}

static Transaction *txSearch(const TxTree *tree, int id)
{
    TxNode *node = tree->root;
    int i;

    if (!node)
        return NULL;
    while (!node->leaf) {
        for (i = 0; i < node->n && id >= node->keys[i]; i++)
            ;
        node = node->child[i];
    }
    for (i = 0; i < node->n; i++)
        if (node->keys[i] == id)
            return node->rec[i];
    return NULL;
}

static TxNode *txFirstLeaf(const TxTree *tree)
{
    TxNode *node = tree->root;
    while (node && !node->leaf)
        node = node->child[0];
    return node;
}

static void txFreeNodes(TxNode *node, bool freeRecords)
{
    int i;
    if (!node)
        return;
    if (node->leaf) {
        if (freeRecords)
            for (i = 0; i < node->n; i++)
                free(node->rec[i]);
    } else {
        for (i = 0; i <= node->n; i++)
            txFreeNodes(node->child[i], freeRecords);
    }
    free(node);
}


/* ================= Seller tree ================= */

static SellerNode *sellerNewNode(bool leaf)
{
    SellerNode *node = allocOrDie(sizeof(SellerNode));
    node->leaf = leaf;
    return node;
}

static SellerNode *sellerInsertRec(SellerNode *node, Seller *s, int *up)
{
    int i, j, mid;
    SellerNode *right, *split;

    if (node->leaf) {
        for (i = node->n; i > 0 && node->keys[i - 1] > s->id; i--) {
            node->keys[i] = node->keys[i - 1];
            node->rec[i] = node->rec[i - 1];
        }
        node->keys[i] = s->id;
        node->rec[i] = s;
        node->n++;
        if (node->n <= MAX_KEYS)
            return NULL;

        mid = node->n / 2;
        right = sellerNewNode(true);
        for (j = mid; j < node->n; j++) {
            right->keys[j - mid] = node->keys[j];
            right->rec[j - mid] = node->rec[j];
        }
        right->n = node->n - mid;
        node->n = mid;
        right->next = node->next;
        node->next = right;
        *up = right->keys[0];
        return right;
    }

    for (i = 0; i < node->n && s->id >= node->keys[i]; i++)
        ;
    split = sellerInsertRec(node->child[i], s, up);
    if (!split)
        return NULL;

    for (j = node->n; j > i; j--) {
        node->keys[j] = node->keys[j - 1];
        node->child[j + 1] = node->child[j];
    }
    node->keys[i] = *up;
    node->child[i + 1] = split;
    node->n++;
    if (node->n <= MAX_KEYS)
        return NULL;

    mid = node->n / 2;
    right = sellerNewNode(false);
    *up = node->keys[mid];
    for (j = mid + 1; j < node->n; j++)
        right->keys[j - mid - 1] = node->keys[j];
    for (j = mid + 1; j <= node->n; j++)
        right->child[j - mid - 1] = node->child[j];
    right->n = node->n - mid - 1;
    node->n = mid;
    return right;
}

static void sellerInsert(SellerTree *tree, Seller *s)
{
    int up;
    SellerNode *split, *root;

    if (!tree->root)
        tree->root = sellerNewNode(true);
    split = sellerInsertRec(tree->root, s, &up);
    if (split) {
        root = sellerNewNode(false);
        root->keys[0] = up;
        root->child[0] = tree->root;
        root->child[1] = split;
        root->n = 1;
        tree->root = root;
    }
    tree->count++;
}

static Seller *sellerSearch(const SellerTree *tree, int id)
{
    SellerNode *node = tree->root;
    int i;

    if (!node)
        return NULL;
    while (!node->leaf) {
        for (i = 0; i < node->n && id >= node->keys[i]; i++)
            ;
        node = node->child[i];
    }
    for (i = 0; i < node->n; i++)
        if (node->keys[i] == id)
            return node->rec[i];
    return NULL;
}

static SellerNode *sellerFirstLeaf(const SellerTree *tree)
{
    SellerNode *node = tree->root;
    while (node && !node->leaf)
        node = node->child[0];
    return node;
}

static void sellerFreeNodes(SellerNode *node)
{
    int i;
    RegularBuyer *r, *nx;

    if (!node)
        return;
    if (node->leaf) {
        for (i = 0; i < node->n; i++) {
            for (r = node->rec[i]->regulars; r; r = nx) {
                nx = r->next;
                free(r);
            }
            txFreeNodes(node->rec[i]->txns.root, false);
            free(node->rec[i]);
        }
    } else {
        for (i = 0; i <= node->n; i++)
            sellerFreeNodes(node->child[i]);
    }
    free(node);
}


/* ================= Buyer tree ================= */

static BuyerNode *buyerNewNode(bool leaf)
{
    BuyerNode *node = allocOrDie(sizeof(BuyerNode));
    node->leaf = leaf;
    return node;
}

static BuyerNode *buyerInsertRec(BuyerNode *node, Buyer *b, int *up)
{
    int i, j, mid;
    BuyerNode *right, *split;

    if (node->leaf) {
        for (i = node->n; i > 0 && node->keys[i - 1] > b->id; i--) {
            node->keys[i] = node->keys[i - 1];
            node->rec[i] = node->rec[i - 1];
        }
        node->keys[i] = b->id;
        node->rec[i] = b;
        node->n++;
        if (node->n <= MAX_KEYS)
            return NULL;

        mid = node->n / 2;
        right = buyerNewNode(true);
        for (j = mid; j < node->n; j++) {
            right->keys[j - mid] = node->keys[j];
            right->rec[j - mid] = node->rec[j];
        }
        right->n = node->n - mid;
        node->n = mid;
        right->next = node->next;
        node->next = right;
        *up = right->keys[0];
        return right;
    }

    for (i = 0; i < node->n && b->id >= node->keys[i]; i++)
        ;
    split = buyerInsertRec(node->child[i], b, up);
    if (!split)
        return NULL;

    for (j = node->n; j > i; j--) {
        node->keys[j] = node->keys[j - 1];
        node->child[j + 1] = node->child[j];
    }
    node->keys[i] = *up;
    node->child[i + 1] = split;
    node->n++;
    if (node->n <= MAX_KEYS)
        return NULL;

    mid = node->n / 2;
    right = buyerNewNode(false);
    *up = node->keys[mid];
    for (j = mid + 1; j < node->n; j++)
        right->keys[j - mid - 1] = node->keys[j];
    for (j = mid + 1; j <= node->n; j++)
        right->child[j - mid - 1] = node->child[j];
    right->n = node->n - mid - 1;
    node->n = mid;
    return right;
}

static void buyerInsert(BuyerTree *tree, Buyer *b)
{
    int up;
    BuyerNode *split, *root;

    if (!tree->root)
        tree->root = buyerNewNode(true);
    split = buyerInsertRec(tree->root, b, &up);
    if (split) {
        root = buyerNewNode(false);
        root->keys[0] = up;
        root->child[0] = tree->root;
        root->child[1] = split;
        root->n = 1;
        tree->root = root;
    }
    tree->count++;
}

static Buyer *buyerSearch(const BuyerTree *tree, int id)
{
    BuyerNode *node = tree->root;
    int i;

    if (!node)
        return NULL;
    while (!node->leaf) {
        for (i = 0; i < node->n && id >= node->keys[i]; i++)
            ;
        node = node->child[i];
    }
    for (i = 0; i < node->n; i++)
        if (node->keys[i] == id)
            return node->rec[i];
    return NULL;
}

static BuyerNode *buyerFirstLeaf(const BuyerTree *tree)
{
    BuyerNode *node = tree->root;
    while (node && !node->leaf)
        node = node->child[0];
    return node;
}

static void buyerFreeNodes(BuyerNode *node)
{
    int i;
    if (!node)
        return;
    if (node->leaf) {
        for (i = 0; i < node->n; i++) {
            txFreeNodes(node->rec[i]->txns.root, false);
            free(node->rec[i]);
        }
    } else {
        for (i = 0; i <= node->n; i++)
            buyerFreeNodes(node->child[i]);
    }
    free(node);
}


/* ================= Seller/buyer pair tree ================= */

static int pairCmp(PairKey a, PairKey b)
{
    if (a.seller_id != b.seller_id)
        return a.seller_id < b.seller_id ? -1 : 1;
    if (a.buyer_id != b.buyer_id)
        return a.buyer_id < b.buyer_id ? -1 : 1;
    return 0;
}

static PairNode *pairNewNode(bool leaf)
{
    PairNode *node = allocOrDie(sizeof(PairNode));
    node->leaf = leaf;
    return node;
}

static PairNode *pairInsertRec(PairNode *node, Pair *p, PairKey key, PairKey *up)
{
    int i, j, mid;
    PairNode *right, *split;

    if (node->leaf) {
        for (i = node->n; i > 0 && pairCmp(node->keys[i - 1], key) > 0; i--) {
            node->keys[i] = node->keys[i - 1];
            node->rec[i] = node->rec[i - 1];
        }
        node->keys[i] = key;
        node->rec[i] = p;
        node->n++;
        if (node->n <= MAX_KEYS)
            return NULL;

        mid = node->n / 2;
        right = pairNewNode(true);
        for (j = mid; j < node->n; j++) {
            right->keys[j - mid] = node->keys[j];
            right->rec[j - mid] = node->rec[j];
        }
        right->n = node->n - mid;
        node->n = mid;
        right->next = node->next;
        node->next = right;
        *up = right->keys[0];
        return right;
    }

    for (i = 0; i < node->n && pairCmp(key, node->keys[i]) >= 0; i++)
        ;
    split = pairInsertRec(node->child[i], p, key, up);
    if (!split)
        return NULL;

    for (j = node->n; j > i; j--) {
        node->keys[j] = node->keys[j - 1];
        node->child[j + 1] = node->child[j];
    }
    node->keys[i] = *up;
    node->child[i + 1] = split;
    node->n++;
    if (node->n <= MAX_KEYS)
        return NULL;

    mid = node->n / 2;
    right = pairNewNode(false);
    *up = node->keys[mid];
    for (j = mid + 1; j < node->n; j++)
        right->keys[j - mid - 1] = node->keys[j];
    for (j = mid + 1; j <= node->n; j++)
        right->child[j - mid - 1] = node->child[j];
    right->n = node->n - mid - 1;
    node->n = mid;
    return right;
}

static void pairInsert(PairTree *tree, Pair *p)
{
    PairKey key = { p->seller_id, p->buyer_id };
    PairKey up;
    PairNode *split, *root;

    if (!tree->root)
        tree->root = pairNewNode(true);
    split = pairInsertRec(tree->root, p, key, &up);
    if (split) {
        root = pairNewNode(false);
        root->keys[0] = up;
        root->child[0] = tree->root;
        root->child[1] = split;
        root->n = 1;
        tree->root = root;
    }
    tree->count++;
}

static Pair *pairSearch(const PairTree *tree, int seller_id, int buyer_id)
{
    PairKey key = { seller_id, buyer_id };
    PairNode *node = tree->root;
    int i;

    if (!node)
        return NULL;
    while (!node->leaf) {
        for (i = 0; i < node->n && pairCmp(key, node->keys[i]) >= 0; i++)
            ;
        node = node->child[i];
    }
    for (i = 0; i < node->n; i++)
        if (pairCmp(node->keys[i], key) == 0)
            return node->rec[i];
    return NULL;
}

static PairNode *pairFirstLeaf(const PairTree *tree)
{
    PairNode *node = tree->root;
    while (node && !node->leaf)
        node = node->child[0];
    return node;
}

static void pairFreeNodes(PairNode *node)
{
    int i;
    if (!node)
        return;
    if (node->leaf) {
        for (i = 0; i < node->n; i++)
            free(node->rec[i]);
    } else {
        for (i = 0; i <= node->n; i++)
            pairFreeNodes(node->child[i]);
    }
    free(node);
}


/* ================= Time index ================= */

static int timeCmp(TimeKey a, TimeKey b)
{
    if (a.ts != b.ts)
        return a.ts < b.ts ? -1 : 1;
    if (a.id != b.id)
        return a.id < b.id ? -1 : 1;
    return 0;
}

static TimeNode *timeNewNode(bool leaf)
{
    TimeNode *node = allocOrDie(sizeof(TimeNode));
    node->leaf = leaf;
    return node;
}

static TimeNode *timeInsertRec(TimeNode *node, Transaction *t, TimeKey key, TimeKey *up)
{
    int i, j, mid;
    TimeNode *right, *split;

    if (node->leaf) {
        for (i = node->n; i > 0 && timeCmp(node->keys[i - 1], key) > 0; i--) {
            node->keys[i] = node->keys[i - 1];
            node->rec[i] = node->rec[i - 1];
        }
        node->keys[i] = key;
        node->rec[i] = t;
        node->n++;
        if (node->n <= MAX_KEYS)
            return NULL;

        mid = node->n / 2;
        right = timeNewNode(true);
        for (j = mid; j < node->n; j++) {
            right->keys[j - mid] = node->keys[j];
            right->rec[j - mid] = node->rec[j];
        }
        right->n = node->n - mid;
        node->n = mid;
        right->next = node->next;
        node->next = right;
        *up = right->keys[0];
        return right;
    }

    for (i = 0; i < node->n && timeCmp(key, node->keys[i]) >= 0; i++)
        ;
    split = timeInsertRec(node->child[i], t, key, up);
    if (!split)
        return NULL;

    for (j = node->n; j > i; j--) {
        node->keys[j] = node->keys[j - 1];
        node->child[j + 1] = node->child[j];
    }
    node->keys[i] = *up;
    node->child[i + 1] = split;
    node->n++;
    if (node->n <= MAX_KEYS)
        return NULL;

    mid = node->n / 2;
    right = timeNewNode(false);
    *up = node->keys[mid];
    for (j = mid + 1; j < node->n; j++)
        right->keys[j - mid - 1] = node->keys[j];
    for (j = mid + 1; j <= node->n; j++)
        right->child[j - mid - 1] = node->child[j];
    right->n = node->n - mid - 1;
    node->n = mid;
    return right;
}

static void timeInsert(TimeTree *tree, Transaction *t)
{
    TimeKey key = { t->timestamp, t->id };
    TimeKey up;
    TimeNode *split, *root;

    if (!tree->root)
        tree->root = timeNewNode(true);
    split = timeInsertRec(tree->root, t, key, &up);
    if (split) {
        root = timeNewNode(false);
        root->keys[0] = up;
        root->child[0] = tree->root;
        root->child[1] = split;
        root->n = 1;
        tree->root = root;
    }
    tree->count++;
}

/* leaf where entries with timestamp >= ts would begin */
static TimeNode *timeFindLeaf(const TimeTree *tree, time_t ts)
{
    TimeKey key = { ts, INT_MIN };
    TimeNode *node = tree->root;
    int i;

    while (node && !node->leaf) {
        for (i = 0; i < node->n && timeCmp(key, node->keys[i]) >= 0; i++)
            ;
        node = node->child[i];
    }
    return node;
}

static void timeFreeNodes(TimeNode *node)
{
    int i;
    if (!node)
        return;
    if (!node->leaf)
        for (i = 0; i <= node->n; i++)
            timeFreeNodes(node->child[i]);
    free(node);
}


/* ================= Energy index ================= */

static int energyCmp(EnergyKey a, EnergyKey b)
{
    if (a.energy < b.energy)
        return -1;
    if (a.energy > b.energy)
        return 1;
    if (a.id != b.id)
        return a.id < b.id ? -1 : 1;
    return 0;
}

static EnergyNode *energyNewNode(bool leaf)
{
    EnergyNode *node = allocOrDie(sizeof(EnergyNode));
    node->leaf = leaf;
    return node;
}

static EnergyNode *energyInsertRec(EnergyNode *node, Transaction *t, EnergyKey key, EnergyKey *up)
{
    int i, j, mid;
    EnergyNode *right, *split;

    if (node->leaf) {
        for (i = node->n; i > 0 && energyCmp(node->keys[i - 1], key) > 0; i--) {
            node->keys[i] = node->keys[i - 1];
            node->rec[i] = node->rec[i - 1];
        }
        node->keys[i] = key;
        node->rec[i] = t;
        node->n++;
        if (node->n <= MAX_KEYS)
            return NULL;

        mid = node->n / 2;
        right = energyNewNode(true);
        for (j = mid; j < node->n; j++) {
            right->keys[j - mid] = node->keys[j];
            right->rec[j - mid] = node->rec[j];
        }
        right->n = node->n - mid;
        node->n = mid;
        right->next = node->next;
        node->next = right;
        *up = right->keys[0];
        return right;
    }

    for (i = 0; i < node->n && energyCmp(key, node->keys[i]) >= 0; i++)
        ;
    split = energyInsertRec(node->child[i], t, key, up);
    if (!split)
        return NULL;

    for (j = node->n; j > i; j--) {
        node->keys[j] = node->keys[j - 1];
        node->child[j + 1] = node->child[j];
    }
    node->keys[i] = *up;
    node->child[i + 1] = split;
    node->n++;
    if (node->n <= MAX_KEYS)
        return NULL;

    mid = node->n / 2;
    right = energyNewNode(false);
    *up = node->keys[mid];
    for (j = mid + 1; j < node->n; j++)
        right->keys[j - mid - 1] = node->keys[j];
    for (j = mid + 1; j <= node->n; j++)
        right->child[j - mid - 1] = node->child[j];
    right->n = node->n - mid - 1;
    node->n = mid;
    return right;
}

static void energyInsert(EnergyTree *tree, Transaction *t)
{
    EnergyKey key = { t->energy, t->id };
    EnergyKey up;
    EnergyNode *split, *root;

    if (!tree->root)
        tree->root = energyNewNode(true);
    split = energyInsertRec(tree->root, t, key, &up);
    if (split) {
        root = energyNewNode(false);
        root->keys[0] = up;
        root->child[0] = tree->root;
        root->child[1] = split;
        root->n = 1;
        tree->root = root;
    }
    tree->count++;
}

static EnergyNode *energyFindLeaf(const EnergyTree *tree, double energy)
{
    EnergyKey key = { energy, INT_MIN };
    EnergyNode *node = tree->root;
    int i;

    while (node && !node->leaf) {
        for (i = 0; i < node->n && energyCmp(key, node->keys[i]) >= 0; i++)
            ;
        node = node->child[i];
    }
    return node;
}

static void energyFreeNodes(EnergyNode *node)
{
    int i;
    if (!node)
        return;
    if (!node->leaf)
        for (i = 0; i <= node->n; i++)
            energyFreeNodes(node->child[i]);
    free(node);
}


/* ================= Records ================= */

static Seller *createSeller(int id, double rate_low, double rate_high)
{
    Seller *s = allocOrDie(sizeof(Seller));
    s->id = id;
    s->rate_low = rate_low;
    s->rate_high = rate_high;
    sellerInsert(&sellers, s);
    return s;
}

static Buyer *getBuyer(int id)
{
    Buyer *b = buyerSearch(&buyers, id);
    if (!b) {
        b = allocOrDie(sizeof(Buyer));
        b->id = id;
        buyerInsert(&buyers, b);
    }
    return b;
}

static double sellerRate(const Seller *s, double energy)
{
    return energy > RATE_LIMIT ? s->rate_high : s->rate_low;
}

static void addRegularBuyer(Seller *s, Pair *p)
{
    RegularBuyer *r = allocOrDie(sizeof(RegularBuyer));
    RegularBuyer **pos = &s->regulars;

    r->buyer_id = p->buyer_id;
    r->pair = p;
    while (*pos && (*pos)->buyer_id < r->buyer_id)
        pos = &(*pos)->next;
    r->next = *pos;
    *pos = r;
}

/* seller must already exist */
static void addTransaction(Transaction *t)
{
    Seller *s = sellerSearch(&sellers, t->seller_id);
    Buyer *b = getBuyer(t->buyer_id);
    Pair *p;

    txInsert(&allTxns, t);
    timeInsert(&timeIndex, t);
    energyInsert(&energyIndex, t);

    txInsert(&s->txns, t);
    txInsert(&b->txns, t);
    b->total_energy += t->energy;

    p = pairSearch(&pairs, t->seller_id, t->buyer_id);
    if (!p) {
        p = allocOrDie(sizeof(Pair));
        p->seller_id = t->seller_id;
        p->buyer_id = t->buyer_id;
        pairInsert(&pairs, p);
    }
    p->count++;
    if (p->count == REGULAR_MIN)
        addRegularBuyer(s, p);

    if (t->id >= nextTxId)
        nextTxId = t->id + 1;
}

static void freeAll(void)
{
    sellerFreeNodes(sellers.root);
    buyerFreeNodes(buyers.root);
    pairFreeNodes(pairs.root);
    timeFreeNodes(timeIndex.root);
    energyFreeNodes(energyIndex.root);
    txFreeNodes(allTxns.root, true);
}


/* ================= Files ================= */

static void saveSellers(void)
{
    FILE *fp = fopen(SELLER_FILE, "w");
    SellerNode *leaf;
    int i;

    if (!fp) {
        printf("Warning: could not write %s\n", SELLER_FILE);
        return;
    }
    fprintf(fp, "seller_id,rate_upto_300,rate_above_300\n");
    for (leaf = sellerFirstLeaf(&sellers); leaf; leaf = leaf->next)
        for (i = 0; i < leaf->n; i++)
            fprintf(fp, "%d,%.2f,%.2f\n", leaf->rec[i]->id,
                    leaf->rec[i]->rate_low, leaf->rec[i]->rate_high);
    fclose(fp);
}

static bool appendTransaction(const Transaction *t)
{
    FILE *fp = fopen(TX_FILE, "a+");
    bool ok;

    if (!fp) {
        printf("Warning: could not write %s\n", TX_FILE);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    if (ftell(fp) == 0) {
        fprintf(fp, "transaction_id,buyer_id,seller_id,energy,price,timestamp\n");
    } else {
        fseek(fp, -1, SEEK_END);
        if (fgetc(fp) != '\n') {
            fseek(fp, 0, SEEK_END);
            fputc('\n', fp);
        }
    }
    fseek(fp, 0, SEEK_END);
    ok = fprintf(fp, "%d,%d,%d,%.2f,%.2f,%ld\n", t->id, t->buyer_id, t->seller_id,
                 t->energy, t->price, (long)t->timestamp) > 0;
    if (fclose(fp) != 0)
        ok = false;
    if (!ok)
        printf("Warning: could not write %s\n", TX_FILE);
    return ok;
}

static char *trimField(char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

/* splits a CSV line into exactly n trimmed fields */
static bool splitFields(char *line, char **f, int n)
{
    int k = 0;
    char *p = line, *comma;

    for (;;) {
        if (k == n)
            return false;
        comma = strchr(p, ',');
        if (comma)
            *comma = '\0';
        f[k++] = trimField(p);
        if (!comma)
            break;
        p = comma + 1;
    }
    return k == n;
}

static bool parseLong(const char *s, long min, long max, long *out)
{
    char *end;
    long v;

    if (!*s)
        return false;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || *end || v < min || v > max)
        return false;
    *out = v;
    return true;
}

/* plain decimal only: [sign] digits [. digits] [e [sign] digits] -- no hex, inf or nan */
static bool isDecimal(const char *s)
{
    bool digits = false;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '+' || *s == '-')
        s++;
    while (isdigit((unsigned char)*s)) {
        s++;
        digits = true;
    }
    if (*s == '.')
        s++;
    while (isdigit((unsigned char)*s)) {
        s++;
        digits = true;
    }
    if (!digits)
        return false;
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-')
            s++;
        if (!isdigit((unsigned char)*s))
            return false;
        while (isdigit((unsigned char)*s))
            s++;
    }
    while (*s == ' ' || *s == '\t')
        s++;
    return *s == '\0';
}

static bool parseAmount(const char *s, double *out)
{
    char *end;
    double v;

    if (!isDecimal(s))
        return false;
    errno = 0;
    v = strtod(s, &end);
    if (errno || *end || !(v > 0 && v < MAX_AMOUNT))
        return false;
    *out = v;
    return true;
}

/* reads one line; false at end of file. Over-long lines are consumed and flagged. */
static bool readFileLine(FILE *fp, char *line, int size, bool *tooLong)
{
    size_t len;

    if (!fgets(line, size, fp))
        return false;
    len = strlen(line);
    *tooLong = false;
    if (len == (size_t)size - 1 && line[len - 1] != '\n') {
        int c;
        while ((c = fgetc(fp)) != '\n' && c != EOF)
            ;
        *tooLong = true;
    }
    return true;
}

static bool isHeaderOrBlank(char *line, bool first)
{
    char *p = line;
    if (first && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
        && (unsigned char)p[2] == 0xBF)
        p += 3;
    p = trimField(p);
    if (*p == '\0')
        return true;
    return first && isalpha((unsigned char)*p);
}

static void loadData(void)
{
    FILE *fp;
    char line[256], *f[6];
    int bad = 0, badSellers = 0, loaded = 0;
    bool newSellers = false, first, tooLong;

    fp = fopen(SELLER_FILE, "r");
    if (fp) {
        first = true;
        while (readFileLine(fp, line, sizeof line, &tooLong)) {
            long id;
            double lo, hi;
            bool header = isHeaderOrBlank(line, first);
            char *text = line;

            if (first && (unsigned char)text[0] == 0xEF)
                text += 3;
            first = false;
            if (header && !tooLong)
                continue;
            if (tooLong || !splitFields(text, f, 3) || !parseLong(f[0], 1, INT_MAX, &id)
                || !parseAmount(f[1], &lo) || !parseAmount(f[2], &hi)
                || sellerSearch(&sellers, (int)id)) {
                badSellers++;
                continue;
            }
            createSeller((int)id, lo, hi);
        }
        fclose(fp);
    }

    fp = fopen(TX_FILE, "r");
    if (!fp) {
        printf("No %s found, starting with an empty record.\n", TX_FILE);
        if (badSellers)
            printf("Skipped %d invalid or duplicate line(s) in %s.\n", badSellers, SELLER_FILE);
        return;
    }
    first = true;
    while (readFileLine(fp, line, sizeof line, &tooLong)) {
        long id, buyer_id, seller_id, ts;
        double energy, price;
        Transaction *t;
        bool header = isHeaderOrBlank(line, first);
        char *text = line;

        if (first && (unsigned char)text[0] == 0xEF)
            text += 3;
        first = false;
        if (header && !tooLong)
            continue;
        if (tooLong || !splitFields(text, f, 6)
            || !parseLong(f[0], 1, INT_MAX - 1, &id)
            || !parseLong(f[1], 1, INT_MAX, &buyer_id)
            || !parseLong(f[2], 1, INT_MAX, &seller_id)
            || !parseAmount(f[3], &energy) || !parseAmount(f[4], &price)
            || !parseLong(f[5], 0, 2147483647L, &ts)
            || txSearch(&allTxns, (int)id)) {
            bad++;
            continue;
        }
        if (!sellerSearch(&sellers, (int)seller_id)) {
            createSeller((int)seller_id, price, price);
            newSellers = true;
        }
        t = allocOrDie(sizeof(Transaction));
        t->id = (int)id;
        t->buyer_id = (int)buyer_id;
        t->seller_id = (int)seller_id;
        t->energy = energy;
        t->price = price;
        t->total = energy * price;
        t->timestamp = (time_t)ts;
        addTransaction(t);
        loaded++;
    }
    fclose(fp);

    if (newSellers)
        saveSellers();
    printf("Loaded %d transactions, %d sellers, %d buyers.\n",
           loaded, sellers.count, buyers.count);
    if (badSellers)
        printf("Skipped %d invalid or duplicate line(s) in %s.\n", badSellers, SELLER_FILE);
    if (bad)
        printf("Skipped %d invalid or duplicate line(s) in %s.\n", bad, TX_FILE);
}


/* ================= Input ================= */

static void readLine(const char *prompt, char *buf, int size)
{
    size_t len;

    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, size, stdin)) {
        printf("\nInput closed. Exiting.\n");
        freeAll();
        exit(0);
    }
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    } else {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
    }
    if (len > 0 && buf[len - 1] == '\r')
        buf[--len] = '\0';
}

static bool isBlank(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return *s == '\0';
}

static int readInt(const char *prompt, int min, int max)
{
    char buf[64], *end;
    long v;

    for (;;) {
        readLine(prompt, buf, sizeof buf);
        errno = 0;
        v = strtol(buf, &end, 10);
        if (end != buf && !errno && isBlank(end) && v >= min && v <= max)
            return (int)v;
        printf("Please enter a whole number between %d and %d.\n", min, max);
    }
}

/* rounded to 2 decimal places; must be > 0 unless allowZero */
static double readAmount(const char *prompt, bool allowZero)
{
    char buf[64], *end;
    double v;

    for (;;) {
        readLine(prompt, buf, sizeof buf);
        v = isDecimal(buf) ? strtod(buf, &end) : -1;
        if (v >= 0 && v < MAX_AMOUNT) {
            v = (double)(long long)(v * 100.0 + 0.5) / 100.0;
            if (v > 0 || allowZero)
                return v;
        }
        printf("Please enter a %s number (up to 2 decimals).\n",
               allowZero ? "non-negative" : "positive");
    }
}

static bool readYesNo(const char *prompt)
{
    char buf[16];
    for (;;) {
        readLine(prompt, buf, sizeof buf);
        if (buf[0] == 'y' || buf[0] == 'Y')
            return true;
        if (buf[0] == 'n' || buf[0] == 'N')
            return false;
        printf("Please enter y or n.\n");
    }
}

static int daysInMonth(int y, int m)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return days[m - 1];
}

/* accepts "YYYY-MM-DD" or "YYYY-MM-DD HH:MM" */
static bool parseDateTime(const char *s, bool endOfDay, time_t *out)
{
    int y, mo, d, h = 0, mi = 0, sec = 0, pos = 0;
    struct tm tm;
    const char *rest;

    if (sscanf(s, "%d-%d-%d%n", &y, &mo, &d, &pos) != 3)
        return false;
    rest = s + pos;
    if (!isBlank(rest)) {
        if (sscanf(rest, "%d:%d%n", &h, &mi, &pos) != 2 || !isBlank(rest + pos))
            return false;
    } else if (endOfDay) {
        h = 23;
        mi = 59;
        sec = 59;
    }
    if (y < 1970 || y > 2037 || mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo)
        || h < 0 || h > 23 || mi < 0 || mi > 59)
        return false;

    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    *out = mktime(&tm);
    return *out != (time_t)-1;
}

static time_t readDateTime(const char *prompt, bool endOfDay, bool allowNow)
{
    char buf[64];
    time_t t;

    for (;;) {
        readLine(prompt, buf, sizeof buf);
        if (allowNow && isBlank(buf))
            return time(NULL);
        if (parseDateTime(buf, endOfDay, &t))
            return t;
        printf("Invalid date. Use YYYY-MM-DD or YYYY-MM-DD HH:MM.\n");
    }
}


/* ================= Display ================= */

static void formatTime(time_t t, char *buf, size_t size)
{
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(buf, size, "%Y-%m-%d %H:%M", tm))
        snprintf(buf, size, "%ld", (long)t);
}

static void printTxHeader(void)
{
    printf("%5s  %6s  %6s  %11s  %8s  %10s  %s\n",
           "TxID", "Buyer", "Seller", "Energy(kWh)", "Rate/kWh", "Total", "Date & Time");
    printf("-----  ------  ------  -----------  --------  ----------  ----------------\n");
}

static void printTx(const Transaction *t)
{
    char when[32];
    formatTime(t->timestamp, when, sizeof when);
    printf("%5d  %6d  %6d  %11.2f  %8.2f  %10.2f  %s\n", t->id, t->buyer_id,
           t->seller_id, t->energy, t->price, t->total, when);
}

static void printTxTree(const TxTree *tree)
{
    TxNode *leaf;
    int i;

    printTxHeader();
    for (leaf = txFirstLeaf(tree); leaf; leaf = leaf->next)
        for (i = 0; i < leaf->n; i++)
            printTx(leaf->rec[i]);
}


/* ================= Operations ================= */

static void opAddTransactions(void)
{
    do {
        int buyer_id, seller_id;
        double energy;
        Seller *s;
        Transaction *t;

        if (nextTxId == INT_MAX) {
            printf("Transaction IDs are used up (last ID is %d).\n", INT_MAX - 1);
            return;
        }

        printf("\n--- New Transaction (ID %d) ---\n", nextTxId);
        buyer_id = readInt("Buyer ID: ", 1, INT_MAX);
        seller_id = readInt("Seller ID: ", 1, INT_MAX);

        s = sellerSearch(&sellers, seller_id);
        if (!s) {
            double lo, hi;
            printf("Seller %d is new. Enter the seller's pricing.\n", seller_id);
            lo = readAmount("Rate per kWh for up to 300 kWh: ", false);
            hi = readAmount("Rate per kWh above 300 kWh: ", false);
            s = createSeller(seller_id, lo, hi);
            saveSellers();
        }

        energy = readAmount("Energy (kWh): ", false);

        t = allocOrDie(sizeof(Transaction));
        t->id = nextTxId;
        t->buyer_id = buyer_id;
        t->seller_id = seller_id;
        t->energy = energy;
        t->price = sellerRate(s, energy);
        t->total = t->energy * t->price;
        t->timestamp = readDateTime("Date & time (YYYY-MM-DD HH:MM, Enter for now): ", false, true);

        addTransaction(t);
        if (appendTransaction(t))
            printf("\nTransaction saved:\n");
        else
            printf("\nTransaction added for this session only (not saved to file):\n");
        printTxHeader();
        printTx(t);
        printf("\n");
    } while (readYesNo("Add another transaction? (y/n): "));
}

static void opDisplayAll(void)
{
    double energy = 0, value = 0;
    TxNode *leaf;
    int i;

    if (!allTxns.count) {
        printf("No transactions recorded.\n");
        return;
    }
    printf("\n===== All Transactions =====\n");
    printTxTree(&allTxns);
    for (leaf = txFirstLeaf(&allTxns); leaf; leaf = leaf->next)
        for (i = 0; i < leaf->n; i++) {
            energy += leaf->rec[i]->energy;
            value += leaf->rec[i]->total;
        }
    printf("\nTransactions: %d   Energy traded: %.2f kWh   Value: %.2f\n",
           allTxns.count, energy, value);
}

static void opSellerSets(void)
{
    SellerNode *leaf;
    RegularBuyer *r;
    int i;

    if (!sellers.count) {
        printf("No sellers recorded.\n");
        return;
    }
    for (leaf = sellerFirstLeaf(&sellers); leaf; leaf = leaf->next) {
        for (i = 0; i < leaf->n; i++) {
            Seller *s = leaf->rec[i];
            printf("\n===== Seller %d =====\n", s->id);
            printf("Rate up to 300 kWh: %.2f   Rate above 300 kWh: %.2f\n",
                   s->rate_low, s->rate_high);
            printf("Regular buyers (more than 5 txns): ");
            if (!s->regulars)
                printf("none");
            for (r = s->regulars; r; r = r->next)
                printf("%d (%d txns)%s", r->buyer_id, r->pair->count, r->next ? ", " : "");
            printf("\nTransactions: %d\n", s->txns.count);
            if (s->txns.count)
                printTxTree(&s->txns);
        }
    }
}

static void opBuyerSets(void)
{
    BuyerNode *leaf;
    int i;

    if (!buyers.count) {
        printf("No buyers recorded.\n");
        return;
    }
    for (leaf = buyerFirstLeaf(&buyers); leaf; leaf = leaf->next) {
        for (i = 0; i < leaf->n; i++) {
            Buyer *b = leaf->rec[i];
            printf("\n===== Buyer %d =====\n", b->id);
            printf("Total energy purchased: %.2f kWh   Transactions: %d\n",
                   b->total_energy, b->txns.count);
            printTxTree(&b->txns);
        }
    }
}

static void opTimePeriod(void)
{
    time_t from, to;
    TimeNode *leaf;
    int i, count = 0;
    double energy = 0, value = 0;
    bool done = false;

    printf("Dates can be YYYY-MM-DD or YYYY-MM-DD HH:MM\n");
    from = readDateTime("From: ", false, false);
    for (;;) {
        to = readDateTime("To:   ", true, false);
        if (to >= from)
            break;
        printf("End must not be before the start.\n");
    }

    printf("\n===== Transactions in Time Period =====\n");
    printTxHeader();
    for (leaf = timeFindLeaf(&timeIndex, from); leaf && !done; leaf = leaf->next) {
        for (i = 0; i < leaf->n; i++) {
            Transaction *t = leaf->rec[i];
            if (t->timestamp < from)
                continue;
            if (t->timestamp > to) {
                done = true;
                break;
            }
            printTx(t);
            count++;
            energy += t->energy;
            value += t->total;
        }
    }
    if (!count)
        printf("(none)\n");
    printf("\nTransactions: %d   Energy: %.2f kWh   Value: %.2f\n", count, energy, value);
}

static void opRevenue(void)
{
    SellerNode *sl;
    TxNode *tl;
    int i, j;
    double grand = 0;

    if (!sellers.count) {
        printf("No sellers recorded.\n");
        return;
    }
    printf("\n===== Total Revenue by Seller =====\n");
    printf("%6s  %6s  %14s  %12s\n", "Seller", "Txns", "Energy(kWh)", "Revenue");
    printf("------  ------  --------------  ------------\n");
    for (sl = sellerFirstLeaf(&sellers); sl; sl = sl->next) {
        for (i = 0; i < sl->n; i++) {
            Seller *s = sl->rec[i];
            double energy = 0, revenue = 0;
            for (tl = txFirstLeaf(&s->txns); tl; tl = tl->next)
                for (j = 0; j < tl->n; j++) {
                    energy += tl->rec[j]->energy;
                    revenue += tl->rec[j]->total;
                }
            printf("%6d  %6d  %14.2f  %12.2f\n", s->id, s->txns.count, energy, revenue);
            grand += revenue;
        }
    }
    printf("------  ------  --------------  ------------\n");
    printf("%-30s  %12.2f\n", "Total", grand);
}

static void opEnergyRange(void)
{
    double lo, hi;
    EnergyNode *leaf;
    int i, count = 0;
    bool done = false;

    lo = readAmount("Minimum energy (kWh): ", true);
    for (;;) {
        hi = readAmount("Maximum energy (kWh): ", true);
        if (hi >= lo)
            break;
        printf("Maximum must not be less than the minimum.\n");
    }

    printf("\n===== Transactions with %.2f to %.2f kWh (ascending) =====\n", lo, hi);
    printTxHeader();
    for (leaf = energyFindLeaf(&energyIndex, lo); leaf && !done; leaf = leaf->next) {
        for (i = 0; i < leaf->n; i++) {
            Transaction *t = leaf->rec[i];
            if (t->energy < lo)
                continue;
            if (t->energy > hi) {
                done = true;
                break;
            }
            printTx(t);
            count++;
        }
    }
    if (!count)
        printf("(none)\n");
    printf("\nTransactions in range: %d\n", count);
}

static int cmpBuyerEnergy(const void *a, const void *b)
{
    const Buyer *x = *(const Buyer *const *)a;
    const Buyer *y = *(const Buyer *const *)b;
    if (x->total_energy != y->total_energy)
        return x->total_energy < y->total_energy ? -1 : 1;
    return x->id - y->id;
}

static int cmpPairCount(const void *a, const void *b)
{
    const Pair *x = *(const Pair *const *)a;
    const Pair *y = *(const Pair *const *)b;
    if (x->count != y->count)
        return x->count - y->count;
    if (x->seller_id != y->seller_id)
        return x->seller_id - y->seller_id;
    return x->buyer_id - y->buyer_id;
}

static bool askDescending(void)
{
    return readInt("Order - 1. Ascending  2. Descending: ", 1, 2) == 2;
}

static void opSortBuyers(void)
{
    Buyer **list;
    BuyerNode *leaf;
    int i, n = 0;
    bool desc;

    if (!buyers.count) {
        printf("No buyers recorded.\n");
        return;
    }
    desc = askDescending();
    list = allocOrDie((size_t)buyers.count * sizeof(Buyer *));
    for (leaf = buyerFirstLeaf(&buyers); leaf; leaf = leaf->next)
        for (i = 0; i < leaf->n; i++)
            list[n++] = leaf->rec[i];
    qsort(list, (size_t)n, sizeof(Buyer *), cmpBuyerEnergy);

    printf("\n===== Buyers by Energy Bought (%s) =====\n", desc ? "descending" : "ascending");
    printf("%4s  %6s  %6s  %14s\n", "Rank", "Buyer", "Txns", "Energy(kWh)");
    printf("----  ------  ------  --------------\n");
    for (i = 0; i < n; i++) {
        Buyer *b = list[desc ? n - 1 - i : i];
        printf("%4d  %6d  %6d  %14.2f\n", i + 1, b->id, b->txns.count, b->total_energy);
    }
    free(list);
}

static void opSortPairs(void)
{
    Pair **list;
    PairNode *leaf;
    int i, n = 0;
    bool desc;

    if (!pairs.count) {
        printf("No transactions recorded.\n");
        return;
    }
    desc = askDescending();
    list = allocOrDie((size_t)pairs.count * sizeof(Pair *));
    for (leaf = pairFirstLeaf(&pairs); leaf; leaf = leaf->next)
        for (i = 0; i < leaf->n; i++)
            list[n++] = leaf->rec[i];
    qsort(list, (size_t)n, sizeof(Pair *), cmpPairCount);

    printf("\n===== Seller/Buyer Pairs by Transactions (%s) =====\n",
           desc ? "descending" : "ascending");
    printf("%6s  %6s  %6s\n", "Seller", "Buyer", "Txns");
    printf("------  ------  ------\n");
    for (i = 0; i < n; i++) {
        Pair *p = list[desc ? n - 1 - i : i];
        printf("%6d  %6d  %6d%s\n", p->seller_id, p->buyer_id, p->count,
               p->count >= REGULAR_MIN ? "   regular" : "");
    }
    printf("\nTotal pairs: %d\n", n);
    free(list);
}


int main(void)
{
    int choice;

    printf("Energy Trading Record Management System\n");
    loadData();

    for (;;) {
        printf("\n========== MENU ==========\n");
        printf("1. Add New Transactions\n");
        printf("2. Display All Transactions\n");
        printf("3. Transactions for Every Seller\n");
        printf("4. Transactions for Every Buyer\n");
        printf("5. Transactions in a Time Period\n");
        printf("6. Total Revenue by Seller\n");
        printf("7. Transactions in an Energy Range\n");
        printf("8. Sort Buyers by Energy Bought\n");
        printf("9. Sort Seller/Buyer Pairs by Transactions\n");
        printf("0. Exit\n");
        choice = readInt("Enter choice: ", 0, 9);

        switch (choice) {
        case 1: opAddTransactions(); break;
        case 2: opDisplayAll(); break;
        case 3: opSellerSets(); break;
        case 4: opBuyerSets(); break;
        case 5: opTimePeriod(); break;
        case 6: opRevenue(); break;
        case 7: opEnergyRange(); break;
        case 8: opSortBuyers(); break;
        case 9: opSortPairs(); break;
        default:
            freeAll();
            printf("Goodbye.\n");
            return 0;
        }
    }
}
