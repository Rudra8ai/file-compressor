/*
 * huffman.c
 *
 * Simple Huffman Coding compressor/decompressor in C.
 *
 * File format for compressed file:
 * [8 bytes] uint64_t original_char_count
 * [256 * 8 bytes] uint64_t frequencies for ASCII 0..255 (in host byte order)
 * [remaining bytes] compressed bitstream (packed MSB-first in each byte)
 *
 * Notes:
 * - This is a teaching-level implementation: readability and clarity prioritized.
 * - Compile: gcc -std=c11 -O2 huffman.c -o huffman
 * - Usage (run executable and follow menu).
 *
 * Author: written to be human-readable and easy to explain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* -------------------------
   Data structures
   ------------------------- */

typedef struct HuffmanNode {
    unsigned char ch;              /* stored character (valid for leaves) */
    uint64_t freq;                 /* frequency / weight */
    struct HuffmanNode *left;
    struct HuffmanNode *right;
} HuffmanNode;

/* Simple min-heap (priority queue) for Huffman nodes */
typedef struct MinHeap {
    HuffmanNode **data;  /* array of pointers to nodes */
    int capacity;
    int size;
} MinHeap;

/* To store codes (as string of '0' and '1') for each char */
typedef struct {
    char *bits;      /* dynamically allocated string e.g. "0101" */
} Code;

/* -------------------------
   Utility functions
   ------------------------- */

static HuffmanNode *create_node(unsigned char ch, uint64_t freq) {
    HuffmanNode *node = (HuffmanNode *)malloc(sizeof(HuffmanNode));
    if (!node) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    node->ch = ch;
    node->freq = freq;
    node->left = node->right = NULL;
    return node;
}

static void free_tree(HuffmanNode *root) {
    if (!root) return;
    free_tree(root->left);
    free_tree(root->right);
    free(root);
}

/* -------------------------
   Min-heap functions
   ------------------------- */

static MinHeap *heap_create(int capacity) {
    MinHeap *h = (MinHeap *)malloc(sizeof(MinHeap));
    if (!h) exit(EXIT_FAILURE);
    h->data = (HuffmanNode **)malloc(sizeof(HuffmanNode *) * capacity);
    h->capacity = capacity;
    h->size = 0;
    return h;
}

static void heap_swap(HuffmanNode **a, HuffmanNode **b) {
    HuffmanNode *tmp = *a;
    *a = *b;
    *b = tmp;
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
    while (1) {
        int left = idx * 2 + 1;
        int right = idx * 2 + 2;
        int smallest = idx;
        if (left < h->size && h->data[left]->freq < h->data[smallest]->freq)
            smallest = left;
        if (right < h->size && h->data[right]->freq < h->data[smallest]->freq)
            smallest = right;
        if (smallest == idx) break;
        heap_swap(&h->data[idx], &h->data[smallest]);
        idx = smallest;
    }
}

static void heap_insert(MinHeap *h, HuffmanNode *node) {
    if (h->size >= h->capacity) {
        /* grow */
        int newCap = h->capacity * 2;
        HuffmanNode **newData = (HuffmanNode **)realloc(h->data, sizeof(HuffmanNode *) * newCap);
        if (!newData) {
            fprintf(stderr, "Heap realloc failed\n");
            exit(EXIT_FAILURE);
        }
        h->data = newData;
        h->capacity = newCap;
    }
    h->data[h->size] = node;
    heapify_up(h, h->size);
    h->size++;
}

static HuffmanNode *heap_extract_min(MinHeap *h) {
    if (h->size == 0) return NULL;
    HuffmanNode *min = h->data[0];
    h->data[0] = h->data[h->size - 1];
    h->size--;
    heapify_down(h, 0);
    return min;
}

static void heap_free(MinHeap *h) {
    free(h->data);
    free(h);
}

/* -------------------------
   Code generation
   ------------------------- */

/* Recursively traverse tree and build codes array */
static void generate_codes_recursive(HuffmanNode *node, char *buffer, int depth, Code codes[256]) {
    if (!node) return;

    /* If leaf, store the code */
    if (!node->left && !node->right) {
        buffer[depth] = '\0';
        codes[node->ch].bits = strdup(buffer);
        if (!codes[node->ch].bits) {
            fprintf(stderr, "strdup failed\n");
            exit(EXIT_FAILURE);
        }
        return;
    }

    /* go left -> '0' */
    if (node->left) {
        buffer[depth] = '0';
        generate_codes_recursive(node->left, buffer, depth + 1, codes);
    }

    /* go right -> '1' */
    if (node->right) {
        buffer[depth] = '1';
        generate_codes_recursive(node->right, buffer, depth + 1, codes);
    }
}

static void generate_codes(HuffmanNode *root, Code codes[256]) {
    char buffer[512]; /* max depth safe for small projects */
    for (int i = 0; i < 256; ++i) {
        codes[i].bits = NULL;
    }
    if (!root) return;
    /* Special case: if tree has only one node (single unique char), assign code "0" */
    if (!root->left && !root->right) {
        buffer[0] = '0';
        buffer[1] = '\0';
        codes[root->ch].bits = strdup(buffer);
        return;
    }
    generate_codes_recursive(root, buffer, 0, codes);
}

/* Free codes */
static void free_codes(Code codes[256]) {
    for (int i = 0; i < 256; ++i) {
        if (codes[i].bits) free(codes[i].bits);
    }
}

/* -------------------------
   Compression helpers
   ------------------------- */

/* Write a single bit into the output stream's buffer; flush to file when full */
typedef struct {
    FILE *fp;
    unsigned char buffer; /* holds bits (packed MSB-first) */
    int bit_count;        /* how many bits currently in buffer (0..7) */
} BitWriter;

static BitWriter *bitwriter_create(FILE *fp) {
    BitWriter *bw = (BitWriter *)malloc(sizeof(BitWriter));
    if (!bw) exit(EXIT_FAILURE);
    bw->fp = fp;
    bw->buffer = 0;
    bw->bit_count = 0;
    return bw;
}

static void bitwriter_write_bit(BitWriter *bw, int bit) {
    /* pack MSB first in byte: next bit goes to bit position 7 - bit_count */
    bw->buffer |= ((bit & 1) << (7 - bw->bit_count));
    bw->bit_count++;
    if (bw->bit_count == 8) {
        fwrite(&bw->buffer, 1, 1, bw->fp);
        bw->buffer = 0;
        bw->bit_count = 0;
    }
}

static void bitwriter_write_bits_from_string(BitWriter *bw, const char *bits) {
    for (const char *p = bits; *p; ++p) {
        bitwriter_write_bit(bw, (*p == '1') ? 1 : 0);
    }
}

static void bitwriter_flush(BitWriter *bw) {
    if (bw->bit_count > 0) {
        fwrite(&bw->buffer, 1, 1, bw->fp);
        bw->buffer = 0;
        bw->bit_count = 0;
    }
    free(bw);
}

/* -------------------------
   Decompression helpers
   ------------------------- */

typedef struct {
    FILE *fp;
    unsigned char buffer;
    int bit_count; /* how many bits left unread in buffer (0..7) */
} BitReader;

static BitReader *bitreader_create(FILE *fp) {
    BitReader *br = (BitReader *)malloc(sizeof(BitReader));
    if (!br) exit(EXIT_FAILURE);
    br->fp = fp;
    br->buffer = 0;
    br->bit_count = 0;
    return br;
}

static int bitreader_read_bit(BitReader *br, int *out_bit) {
    if (br->bit_count == 0) {
        size_t r = fread(&br->buffer, 1, 1, br->fp);
        if (r != 1) {
            return 0; /* EOF */
        }
        br->bit_count = 8;
    }
    /* extract MSB-first */
    int pos = br->bit_count - 1;
    int bit = (br->buffer >> (7 - pos)) & 1;
    br->bit_count--;
    *out_bit = bit;
    return 1;
}

static void bitreader_free(BitReader *br) {
    free(br);
}

/* -------------------------
   Huffman build / encode / decode
   ------------------------- */

/* Build Huffman tree from frequency table */
static HuffmanNode *build_huffman_tree(uint64_t frequencies[256]) {
    /* count how many characters have freq > 0 */
    int uniqueCount = 0;
    for (int i = 0; i < 256; ++i) if (frequencies[i] > 0) uniqueCount++;

    if (uniqueCount == 0) return NULL;

    MinHeap *heap = heap_create(uniqueCount * 2 + 4);

    /* create leaf nodes */
    for (int i = 0; i < 256; ++i) {
        if (frequencies[i] > 0) {
            HuffmanNode *n = create_node((unsigned char)i, frequencies[i]);
            heap_insert(heap, n);
        }
    }

    /* special case: only one unique char -> return single node */
    if (heap->size == 1) {
        HuffmanNode *single = heap_extract_min(heap);
        heap_free(heap);
        return single;
    }

    /* combine two smallest until one tree remains */
    while (heap->size > 1) {
        HuffmanNode *left = heap_extract_min(heap);
        HuffmanNode *right = heap_extract_min(heap);
        HuffmanNode *parent = create_node(0, left->freq + right->freq);
        parent->left = left;
        parent->right = right;
        heap_insert(heap, parent);
    }

    HuffmanNode *root = heap_extract_min(heap);
    heap_free(heap);
    return root;
}

/* Compress file: read input, count frequencies, build codes, write header and compressed bits */
static int compress_file(const char *input_path, const char *output_path) {
    FILE *in = fopen(input_path, "rb");
    if (!in) {
        fprintf(stderr, "Error: cannot open input file '%s'\n", input_path);
        return 0;
    }

    /* frequency table */
    uint64_t frequencies[256];
    for (int i = 0; i < 256; ++i) frequencies[i] = 0;

    /* First pass: count frequencies and measure total chars */
    int c;
    uint64_t total_chars = 0;
    while ((c = fgetc(in)) != EOF) {
        frequencies[(unsigned char)c]++;
        total_chars++;
    }

    /* Build tree */
    HuffmanNode *root = build_huffman_tree(frequencies);
    if (!root) {
        fprintf(stderr, "Input file is empty. Nothing to compress.\n");
        fclose(in);
        return 0;
    }

    /* Generate codes */
    Code codes[256];
    generate_codes(root, codes);

    /* Open output and write header: total_chars + frequencies[256] */
    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot open output file '%s'\n", output_path);
        free_tree(root);
        free_codes(codes);
        fclose(in);
        return 0;
    }

    /* write original character count (8 bytes) */
    if (fwrite(&total_chars, sizeof(uint64_t), 1, out) != 1) {
        fprintf(stderr, "Error writing header\n");
        fclose(in); fclose(out);
        free_tree(root); free_codes(codes);
        return 0;
    }

    /* write frequency table (256 * 8 bytes) */
    if (fwrite(frequencies, sizeof(uint64_t), 256, out) != 256) {
        fprintf(stderr, "Error writing frequency table\n");
        fclose(in); fclose(out);
        free_tree(root); free_codes(codes);
        return 0;
    }

    /* Second pass: encode and write compressed bits */
    rewind(in);
    BitWriter *bw = bitwriter_create(out);
    while ((c = fgetc(in)) != EOF) {
        unsigned char uc = (unsigned char)c;
        const char *bits = codes[uc].bits;
        if (!bits) {
            fprintf(stderr, "Error: no code for character %u\n", uc);
            bitwriter_flush(bw); fclose(in); fclose(out);
            free_tree(root); free_codes(codes);
            return 0;
        }
        bitwriter_write_bits_from_string(bw, bits);
    }

    /* flush remaining bits */
    bitwriter_flush(bw);
    fclose(in);
    fclose(out);

    /* cleanup */
    free_codes(codes);
    free_tree(root);

    /* success */
    return 1;
}

/* Decompress file: read header, rebuild tree, decode bits and write original data */
static int decompress_file(const char *input_path, const char *output_path) {
    FILE *in = fopen(input_path, "rb");
    if (!in) {
        fprintf(stderr, "Error: cannot open compressed file '%s'\n", input_path);
        return 0;
    }

    uint64_t total_chars;
    uint64_t frequencies[256];

    /* read header */
    if (fread(&total_chars, sizeof(uint64_t), 1, in) != 1) {
        fprintf(stderr, "Error: cannot read original size from header\n");
        fclose(in);
        return 0;
    }

    if (fread(frequencies, sizeof(uint64_t), 256, in) != 256) {
        fprintf(stderr, "Error: cannot read frequency table from header\n");
        fclose(in);
        return 0;
    }

    /* rebuild huffman tree from frequency table */
    HuffmanNode *root = build_huffman_tree(frequencies);
    if (!root && total_chars > 0) {
        fprintf(stderr, "Error rebuilding Huffman tree\n");
        fclose(in);
        return 0;
    }

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot open output file '%s'\n", output_path);
        fclose(in);
        free_tree(root);
        return 0;
    }

    /* Special case: if only one unique char, write it total_chars times */
    int uniqueCount = 0;
    unsigned char onlyChar = 0;
    for (int i = 0; i < 256; ++i) {
        if (frequencies[i] > 0) {
            uniqueCount++;
            onlyChar = (unsigned char)i;
        }
    }
    if (uniqueCount == 1) {
        for (uint64_t i = 0; i < total_chars; ++i) {
            fputc(onlyChar, out);
        }
        fclose(in); fclose(out);
        free_tree(root);
        return 1;
    }

    /* regular decoding: read bits and traverse tree */
    BitReader *br = bitreader_create(in);
    uint64_t written = 0;
    HuffmanNode *node = root;
    while (written < total_chars) {
        int bit;
        if (!bitreader_read_bit(br, &bit)) {
            /* Unexpected EOF */
            fprintf(stderr, "Unexpected end of compressed file\n");
            break;
        }
        if (bit == 0) node = node->left;
        else node = node->right;

        if (!node->left && !node->right) {
            /* leaf */
            fputc(node->ch, out);
            written++;
            node = root; /* restart for next symbol */
        }
    }

    bitreader_free(br);
    fclose(in);
    fclose(out);
    free_tree(root);

    return (written == total_chars);
}

/* -------------------------
   Small helpers for user
   ------------------------- */

static void print_menu(void) {
    printf("Huffman Compressor\n");
    printf("------------------\n");
    printf("1. Compress a file\n");
    printf("2. Decompress a file\n");
    printf("3. Exit\n");
    printf("Enter choice: ");
}

/* -------------------------
   Main
   ------------------------- */

int main(void) {
    while (1) {
        print_menu();
        int choice;
        if (scanf("%d", &choice) != 1) {
            /* clear input */
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            continue;
        }
        if (choice == 1) {
            char input_path[512], output_path[512];
            printf("Enter input file path to compress: ");
            scanf("%511s", input_path);
            printf("Enter output compressed file path (e.g. out.huf): ");
            scanf("%511s", output_path);
            printf("Compressing...\n");
            if (compress_file(input_path, output_path)) {
                printf("Compression successful: '%s' -> '%s'\n", input_path, output_path);
            } else {
                printf("Compression failed.\n");
            }
        } else if (choice == 2) {
            char input_path[512], output_path[512];
            printf("Enter compressed file path to decompress: ");
            scanf("%511s", input_path);
            printf("Enter output decompressed file path (e.g. out.txt): ");
            scanf("%511s", output_path);
            printf("Decompressing...\n");
            if (decompress_file(input_path, output_path)) {
                printf("Decompression successful: '%s' -> '%s'\n", input_path, output_path);
            } else {
                printf("Decompression failed.\n");
            }
        } else if (choice == 3) {
            printf("Exiting.\n");
            break;
        } else {
            printf("Invalid choice. Try again.\n");
        }
        /* clear trailing newline so next scanf works reliably */
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {}
    }
    return 0;
}
