/*
 * huffman_tool.c
 *
 * File Compression & Decompression Tool using Huffman Coding
 *
 * - Read a text (or binary) file, count byte frequencies (0..255)
 * - Build Huffman tree, create codes for each byte
 * - Compress into a file: header (total bytes + 256 freqs) + packed bitstream
 * - Decompress by rebuilding the tree and decoding bits
 *
 * Features:
 * - Menu-driven console UI
 * - Option to display Huffman codes (for debugging/demo)
 * - Compression ratio display
 * - Safe, modular and commented (student-style)
 *
 * Compile:
 *   gcc -std=c11 -O2 huffman_tool.c -o huffman_tool
 *
 * Run:
 *   ./huffman_tool
 *
 * Author: student-friendly style
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* -----------------------------
   Data structures & typedefs
   ----------------------------- */

typedef struct HuffmanNode {
    unsigned char ch;         /* character stored (valid in leaves) */
    uint64_t freq;            /* frequency / weight */
    struct HuffmanNode *left;
    struct HuffmanNode *right;
} HuffmanNode;

typedef struct {
    HuffmanNode **data;
    int size;
    int capacity;
} MinHeap;

/* Each code stored as a null-terminated string of '0' and '1' */
typedef struct {
    char *bits; /* dynamically allocated string */
} Code;

/* Bit writer for packing bits into bytes (MSB-first) */
typedef struct {
    FILE *fp;
    unsigned char buffer;
    int bit_count; /* number of bits currently in buffer (0..7) */
} BitWriter;

typedef struct {
    FILE *fp;
    unsigned char buffer;
    int bit_count; /* number of bits remaining in buffer (0..7) */
} BitReader;

/* -----------------------------
   Utility helpers
   ----------------------------- */

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) {
        fprintf(stderr, "strdup failed\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

/* -----------------------------
   Min-heap (priority queue)
   ----------------------------- */

static MinHeap *heap_create(int capacity) {
    MinHeap *h = xmalloc(sizeof(MinHeap));
    h->data = xmalloc(sizeof(HuffmanNode *) * capacity);
    h->size = 0;
    h->capacity = capacity;
    return h;
}

static void heap_free(MinHeap *h) {
    if (!h) return;
    free(h->data);
    free(h);
}

static void heap_swap(HuffmanNode **a, HuffmanNode **b) {
    HuffmanNode *t = *a; *a = *b; *b = t;
}

static void heapify_up(MinHeap *h, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (h->data[parent]->freq <= h->data[idx]->freq) break;
        heap_swap(&h->data[parent], &h->data[idx]);
        idx = parent;
    }
}

static void heapify_down(MinHeap *h, int idx) {
    for (;;) {
        int left = 2*idx + 1;
        int right = 2*idx + 2;
        int smallest = idx;
        if (left < h->size && h->data[left]->freq < h->data[smallest]->freq) smallest = left;
        if (right < h->size && h->data[right]->freq < h->data[smallest]->freq) smallest = right;
        if (smallest == idx) break;
        heap_swap(&h->data[idx], &h->data[smallest]);
        idx = smallest;
    }
}

static void heap_insert(MinHeap *h, HuffmanNode *node) {
    if (h->size >= h->capacity) {
        int newcap = h->capacity * 2;
        if (newcap < 16) newcap = 16;
        h->data = realloc(h->data, sizeof(HuffmanNode *) * newcap);
        if (!h->data) { fprintf(stderr, "Heap realloc failed\n"); exit(EXIT_FAILURE); }
        h->capacity = newcap;
    }
    h->data[h->size++] = node;
    heapify_up(h, h->size - 1);
}

static HuffmanNode *heap_extract_min(MinHeap *h) {
    if (h->size == 0) return NULL;
    HuffmanNode *min = h->data[0];
    h->data[0] = h->data[--h->size];
    heapify_down(h, 0);
    return min;
}

/* -----------------------------
   Huffman tree helpers
   ----------------------------- */

static HuffmanNode *node_create(unsigned char ch, uint64_t freq) {
    HuffmanNode *n = xmalloc(sizeof(HuffmanNode));
    n->ch = ch;
    n->freq = freq;
    n->left = n->right = NULL;
    return n;
}

static void free_tree(HuffmanNode *root) {
    if (!root) return;
    free_tree(root->left);
    free_tree(root->right);
    free(root);
}

/* Build Huffman tree from frequency table (256 entries). Returns root or NULL if empty */
static HuffmanNode *build_huffman_tree(uint64_t freq[256]) {
    int unique = 0;
    for (int i = 0; i < 256; ++i) if (freq[i] > 0) unique++;

    if (unique == 0) return NULL;

    MinHeap *heap = heap_create(unique * 2 + 4);
    for (int i = 0; i < 256; ++i) {
        if (freq[i] > 0) {
            HuffmanNode *leaf = node_create((unsigned char)i, freq[i]);
            heap_insert(heap, leaf);
        }
    }

    /* special case: only one unique char */
    if (heap->size == 1) {
        HuffmanNode *single = heap_extract_min(heap);
        heap_free(heap);
        return single;
    }

    while (heap->size > 1) {
        HuffmanNode *a = heap_extract_min(heap);
        HuffmanNode *b = heap_extract_min(heap);
        HuffmanNode *parent = node_create(0, a->freq + b->freq);
        parent->left = a;
        parent->right = b;
        heap_insert(heap, parent);
    }

    HuffmanNode *root = heap_extract_min(heap);
    heap_free(heap);
    return root;
}

/* -----------------------------
   Code generation
   ----------------------------- */

/* Recursively walk tree and fill codes */
static void generate_codes_recursive(HuffmanNode *node, char *buf, int depth, Code codes[256]) {
    if (!node) return;
    if (!node->left && !node->right) {
        buf[depth] = '\0';
        codes[node->ch].bits = xstrdup(buf);
        return;
    }
    if (node->left) {
        buf[depth] = '0';
        generate_codes_recursive(node->left, buf, depth + 1, codes);
    }
    if (node->right) {
        buf[depth] = '1';
        generate_codes_recursive(node->right, buf, depth + 1, codes);
    }
}

static void generate_codes(HuffmanNode *root, Code codes[256]) {
    for (int i = 0; i < 256; ++i) codes[i].bits = NULL;
    if (!root) return;

    /* if single node, give it code "0" */
    if (!root->left && !root->right) {
        codes[root->ch].bits = xstrdup("0");
        return;
    }
    char buf[512];
    generate_codes_recursive(root, buf, 0, codes);
}

static void free_codes(Code codes[256]) {
    for (int i = 0; i < 256; ++i) {
        if (codes[i].bits) free(codes[i].bits);
        codes[i].bits = NULL;
    }
}

/* -----------------------------
   Bit writing / reading
   ----------------------------- */

static BitWriter *bitwriter_create(FILE *fp) {
    BitWriter *bw = xmalloc(sizeof(BitWriter));
    bw->fp = fp;
    bw->buffer = 0;
    bw->bit_count = 0;
    return bw;
}

/* Write single bit (0 or 1) MSB-first */
static void bitwriter_write_bit(BitWriter *bw, int bit) {
    bw->buffer |= ((bit & 1) << (7 - bw->bit_count));
    bw->bit_count++;
    if (bw->bit_count == 8) {
        fwrite(&bw->buffer, 1, 1, bw->fp);
        bw->buffer = 0;
        bw->bit_count = 0;
    }
}

static void bitwriter_write_bits_from_string(BitWriter *bw, const char *s) {
    for (const char *p = s; *p; ++p) bitwriter_write_bit(bw, (*p == '1'));
}

static void bitwriter_flush(BitWriter *bw) {
    if (bw->bit_count > 0) {
        fwrite(&bw->buffer, 1, 1, bw->fp);
        bw->buffer = 0;
        bw->bit_count = 0;
    }
    free(bw);
}

static BitReader *bitreader_create(FILE *fp) {
    BitReader *br = xmalloc(sizeof(BitReader));
    br->fp = fp;
    br->buffer = 0;
    br->bit_count = 0;
    return br;
}

/* Read one bit: return 1 on success (and set *out), 0 on EOF/error */
static int bitreader_read_bit(BitReader *br, int *out) {
    if (br->bit_count == 0) {
        size_t r = fread(&br->buffer, 1, 1, br->fp);
        if (r != 1) return 0;
        br->bit_count = 8;
    }
    int pos = br->bit_count - 1;
    int bit = (br->buffer >> (7 - pos)) & 1;
    br->bit_count--;
    *out = bit;
    return 1;
}

static void bitreader_free(BitReader *br) {
    free(br);
}

/* -----------------------------
   File I/O: compression & decompression
   ----------------------------- */

/*
 * Compressed file format:
 * [8 bytes] uint64_t total_original_bytes
 * [256 * 8 bytes] uint64_t frequencies[256]
 * [N bytes] packed compressed bitstream (MSB-first in each byte)
 *
 * Note: uses host byte order. For cross-platform transfers, convert to network byte order.
 */

/* Compress input_path into output_path. Returns 1 on success, 0 otherwise */
static int compress_file(const char *input_path, const char *output_path) {
    FILE *in = fopen(input_path, "rb");
    if (!in) {
        fprintf(stderr, "Error: cannot open input file '%s'\n", input_path);
        return 0;
    }

    uint64_t frequencies[256] = {0};
    int ch;
    uint64_t total = 0;

    /* First pass: frequency count */
    while ((ch = fgetc(in)) != EOF) {
        frequencies[(unsigned char)ch]++;
        total++;
    }
    if (total == 0) {
        fprintf(stderr, "Input file '%s' is empty. Nothing to compress.\n", input_path);
        fclose(in);
        return 0;
    }

    HuffmanNode *root = build_huffman_tree(frequencies);
    if (!root) {
        fprintf(stderr, "Failed to build Huffman tree\n");
        fclose(in);
        return 0;
    }

    Code codes[256];
    generate_codes(root, codes);

    /* Open output and write header */
    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot open output file '%s'\n", output_path);
        free_tree(root); free_codes(codes); fclose(in);
        return 0;
    }

    /* header */
    if (fwrite(&total, sizeof(uint64_t), 1, out) != 1) {
        fprintf(stderr, "Error writing header\n");
        fclose(in); fclose(out); free_tree(root); free_codes(codes);
        return 0;
    }
    if (fwrite(frequencies, sizeof(uint64_t), 256, out) != 256) {
        fprintf(stderr, "Error writing frequency table\n");
        fclose(in); fclose(out); free_tree(root); free_codes(codes);
        return 0;
    }

    /* Second pass: encode */
    rewind(in);
    BitWriter *bw = bitwriter_create(out);
    while ((ch = fgetc(in)) != EOF) {
        unsigned char uc = (unsigned char)ch;
        const char *bits = codes[uc].bits;
        if (!bits) { /* shouldn't happen */
            fprintf(stderr, "No code for byte %u\n", uc);
            bitwriter_flush(bw); fclose(in); fclose(out);
            free_tree(root); free_codes(codes);
            return 0;
        }
        bitwriter_write_bits_from_string(bw, bits);
    }
    bitwriter_flush(bw);

    /* show compression ratio (get sizes) */
    fflush(out);
    fclose(in);
    fclose(out);

    /* clean up */
    free_tree(root);
    free_codes(codes);
    return 1;
}

/* Decompress input_path into output_path. Returns 1 on success, 0 otherwise */
static int decompress_file(const char *input_path, const char *output_path) {
    FILE *in = fopen(input_path, "rb");
    if (!in) {
        fprintf(stderr, "Error: cannot open compressed file '%s'\n", input_path);
        return 0;
    }

    uint64_t total = 0;
    uint64_t frequencies[256];

    if (fread(&total, sizeof(uint64_t), 1, in) != 1) {
        fprintf(stderr, "Error: cannot read original size\n");
        fclose(in); return 0;
    }
    if (fread(frequencies, sizeof(uint64_t), 256, in) != 256) {
        fprintf(stderr, "Error: cannot read frequency table\n");
        fclose(in); return 0;
    }

    HuffmanNode *root = build_huffman_tree(frequencies);
    if (!root && total > 0) {
        fprintf(stderr, "Error: rebuilt empty Huffman tree\n");
        fclose(in); return 0;
    }

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot open output file '%s'\n", output_path);
        fclose(in); free_tree(root); return 0;
    }

    /* special case: only one unique char */
    int unique = 0; unsigned char onlyChar = 0;
    for (int i = 0; i < 256; ++i) if (frequencies[i] > 0) { unique++; onlyChar = (unsigned char)i; }
    if (unique == 1) {
        for (uint64_t i = 0; i < total; ++i) fputc(onlyChar, out);
        fclose(in); fclose(out); free_tree(root);
        return 1;
    }

    /* normal case: read bits and traverse tree */
    BitReader *br = bitreader_create(in);
    HuffmanNode *cur = root;
    uint64_t written = 0;
    while (written < total) {
        int bit;
        if (!bitreader_read_bit(br, &bit)) {
            fprintf(stderr, "Unexpected end of compressed file (decoded %llu of %llu)\n",
                    (unsigned long long)written, (unsigned long long)total);
            break;
        }
        if (bit == 0) cur = cur->left;
        else cur = cur->right;

        if (!cur->left && !cur->right) {
            fputc(cur->ch, out);
            written++;
            cur = root;
        }
    }

    bitreader_free(br);
    fclose(in);
    fclose(out);
    free_tree(root);
    return (written == total);
}

/* -----------------------------
   Small helpers & UI
   ----------------------------- */

static void print_codes(Code codes[256]) {
    printf("Huffman Codes (byte -> code):\n");
    for (int i = 0; i < 256; ++i) {
        if (codes[i].bits) {
            /* print printable representation for common bytes */
            if (i >= 32 && i <= 126) printf("'%c' (ASCII %d) : %s\n", (char)i, i, codes[i].bits);
            else printf("0x%02X (ASCII %d) : %s\n", i, i, codes[i].bits);
        }
    }
}

/* Helper to compute file size (0 on error) */
static uint64_t file_size_bytes(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long pos = ftell(f);
    fclose(f);
    if (pos < 0) return 0;
    return (uint64_t)pos;
}

/* Create sample file if missing (small convenience for demos) */
static int create_sample_file_if_missing(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; } /* exists */
    f = fopen(path, "wb");
    if (!f) return 0;
    const char *sample = "This is a sample file for Huffman compression demonstration.\n"
                         "You can replace this with any text file.\n";
    fwrite(sample, 1, strlen(sample), f);
    fclose(f);
    return 1;
}

static void show_menu(void) {
    printf("\n-------- Huffman Compressor --------\n");
    printf("1. Compress a file\n");
    printf("2. Decompress a file\n");
    printf("3. Compress sample file (creates sample if missing)\n");
    printf("4. Exit\n");
    printf("Enter choice: ");
}

/* A helper to build codes just to display them without writing output (for option) */
static void build_and_show_codes_for_input(const char *input_path) {
    FILE *in = fopen(input_path, "rb");
    if (!in) { fprintf(stderr, "Cannot open '%s' to build codes\n", input_path); return; }
    uint64_t freq[256] = {0};
    int c; uint64_t total = 0;
    while ((c = fgetc(in)) != EOF) { freq[(unsigned char)c]++; total++; }
    fclose(in);
    if (total == 0) { printf("File is empty.\n"); return; }
    HuffmanNode *root = build_huffman_tree(freq);
    Code codes[256];
    generate_codes(root, codes);
    print_codes(codes);
    free_codes(codes);
    free_tree(root);
}

/* -----------------------------
   Main program
   ----------------------------- */

int main(void) {
    for (;;) {
        show_menu();
        int choice;
        if (scanf("%d", &choice) != 1) {
            /* clear input */
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            continue;
        }
        if (choice == 1) {
            char inpath[512], outpath[512];
            printf("Enter input file path to compress: ");
            scanf("%511s", inpath);
            printf("Enter output compressed file path (e.g. out.huf): ");
            scanf("%511s", outpath);

            uint64_t before = file_size_bytes(inpath);
            if (before == 0) {
                printf("Warning: input file not found or empty.\n");
                continue;
            }

            printf("Compressing '%s' -> '%s' ...\n", inpath, outpath);
            if (compress_file(inpath, outpath)) {
                uint64_t after = file_size_bytes(outpath);
                if (after == 0) after = 1; /* avoid div/0 */
                double ratio = 100.0 * (1.0 - ((double)after / (double)before));
                printf("Compression successful.\n");
                printf("Original size: %llu bytes, Compressed size: %llu bytes\n",
                       (unsigned long long)before, (unsigned long long)after);
                printf("Space saved: %.2f%%\n", ratio);
                /* Offer to show codes */
                printf("Would you like to view Huffman codes for this file? (y/n): ");
                char ans = 'n';
                while ((getchar()) != '\n') {}
                ans = getchar();
                while ((getchar()) != '\n') {}
                if (ans == 'y' || ans == 'Y') build_and_show_codes_for_input(inpath);
            } else {
                printf("Compression failed.\n");
            }
        } else if (choice == 2) {
            char inpath[512], outpath[512];
            printf("Enter compressed file path to decompress: ");
            scanf("%511s", inpath);
            printf("Enter output decompressed file path (e.g. out.txt): ");
            scanf("%511s", outpath);
            printf("Decompressing '%s' -> '%s' ...\n", inpath, outpath);
            if (decompress_file(inpath, outpath)) {
                printf("Decompression successful.\n");
            } else {
                printf("Decompression failed.\n");
            }
        } else if (choice == 3) {
            char sample_path[512], outpath[512];
            printf("Enter sample input file path to create/use (e.g. sample.txt): ");
            scanf("%511s", sample_path);
            if (!create_sample_file_if_missing(sample_path)) {
                printf("Failed to create sample file.\n"); continue;
            }
            printf("Enter compressed output path (e.g. sample.huf): ");
            scanf("%511s", outpath);
            uint64_t before = file_size_bytes(sample_path);
            if (compress_file(sample_path, outpath)) {
                uint64_t after = file_size_bytes(outpath);
                double ratio = 100.0 * (1.0 - ((double)after / (double)before));
                printf("Sample compressed. Original: %llu, Compressed: %llu, Saved: %.2f%%\n",
                       (unsigned long long)before, (unsigned long long)after, ratio);
            } else {
                printf("Compression of sample failed.\n");
            }
        } else if (choice == 4) {
            printf("Exiting.\n");
            break;
        } else {
            printf("Invalid choice, try again.\n");
        }
        /* clear stdin leftover */
        int ch; while ((ch = getchar()) != '\n' && ch != EOF) {}
    }

    return 0;
}

