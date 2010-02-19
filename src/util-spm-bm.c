/**
 * Copyright (c) 2009 Open Information Security Foundation
 *
 * \author Pablo Rincon Crespo <pablo.rincon.crespo@gmail.com>
 *
 * Boyer Moore algorithm has a really good performance. It need to arrays
 * of context for each pattern that hold applicable shifts on the text
 * to seach in, bassed on characters not available in the pattern
 * and combinations of characters that start a sufix on the pattern.
 * If possible, we should store the context of patterns that we are going
 * to search for multiple times, so we don't spend time on rebuilding them.
 */

#include "suricata-common.h"
#include "suricata.h"
#include "util-spm-bm.h"
#include <time.h>
#include <limits.h>
#include <string.h>

/**
 * \brief Array setup function for bad characters that split the pattern
 *        Remember that the result array should be the length of ALPHABET_SIZE
 *
 * \param str pointer to the pattern string
 * \param size length of the string
 * \param result pointer to an empty array that will hold the badchars
 */
inline void PreBmBc(const uint8_t *x, int32_t m, int32_t *bmBc) {
    int32_t i;

    for (i = 0; i < 256; ++i) {
        bmBc[i] = m;
    }
    for (i = 0; i < m - 1; ++i) {
        bmBc[(unsigned char)x[i]] = m - i - 1;
    }
}

/**
 * \brief Array setup function for building prefixes (shift for valid prefixes) for boyermoore context
 *
 * \param x pointer to the pattern string
 * \param m length of the string
 * \param suff pointer to an empty array that will hold the prefixes (shifts)
 */
inline void BoyerMooreSuffixes(const uint8_t *x, int32_t m, int32_t *suff) {
    int32_t f = 0, g, i;
    suff[m - 1] = m;
    g = m - 1;
    for (i = m - 2; i >= 0; --i) {
        if (i > g && suff[i + m - 1 - f] < i - g)
            suff[i] = suff[i + m - 1 - f];
        else {
            if (i < g)
                g = i;
            f = i;
            while (g >= 0 && x[g] == x[g + m - 1 - f])
                --g;
            suff[i] = f - g;
        }
    }
}

/**
 * \brief Array setup function for building prefixes (shift for valid prefixes) for boyermoore context
 *
 * \param x pointer to the pattern string
 * \param m length of the string
 * \param bmGs pointer to an empty array that will hold the prefixes (shifts)
 */
inline void PreBmGs(const uint8_t *x, int32_t m, int32_t *bmGs) {
    int32_t i, j;
    int32_t *suff;

    suff = SCMalloc(sizeof(int32_t) * (m + 1));

    BoyerMooreSuffixes(x, m, suff);

    for (i = 0; i < m; ++i)
        bmGs[i] = m;

    j = 0;

    for (i = m - 1; i >= -1; --i)
        if (i == -1 || suff[i] == i + 1)
            for (; j < m - 1 - i; ++j)
                if (bmGs[j] == m)
                    bmGs[j] = m - 1 - i;

    for (i = 0; i <= m - 2; ++i)
        bmGs[m - 1 - suff[i]] = m - 1 - i;
    SCFree(suff);
}

/**
 * \brief Array setup function for bad characters that split the pattern
 *        Remember that the result array should be the length of ALPHABET_SIZE
 *
 * \param str pointer to the pattern string
 * \param size length of the string
 * \param result pointer to an empty array that will hold the badchars
 */
inline void PreBmBcNocase(const uint8_t *x, int32_t m, int32_t *bmBc) {
    int32_t i;

    for (i = 0; i < 256; ++i) {
        bmBc[i] = m;
    }
    for (i = 0; i < m - 1; ++i) {
        bmBc[u8_tolower((unsigned char)x[i])] = m - 1 - i;
    }
}

inline void BoyerMooreSuffixesNocase(const uint8_t *x, int32_t m, int32_t *suff) {
    int32_t f = 0, g, i;

    suff[m - 1] = m;
    g = m - 1;
    for (i = m - 2; i >= 0; --i) {
        if (i > g && suff[i + m - 1 - f] < i - g) {
            suff[i] = suff[i + m - 1 - f];
        } else {
            if (i < g) {
                g = i;
            }
            f = i;
            while (g >= 0 && u8_tolower(x[g]) == u8_tolower(x[g + m - 1 - f])) {
                --g;
            }
            suff[i] = f - g;
        }
    }
}

/**
 * \brief Array setup function for building prefixes (shift for valid prefixes)
 *        for boyermoore context case less
 *
 * \param x pointer to the pattern string
 * \param m length of the string
 * \param bmGs pointer to an empty array that will hold the prefixes (shifts)
 */
inline void PreBmGsNocase(const uint8_t *x, int32_t m, int32_t *bmGs) {
    int32_t i, j;
    int32_t* suff;

    suff = SCMalloc(sizeof(int32_t) * (m + 1));

    BoyerMooreSuffixesNocase(x, m, suff);

    for (i = 0; i < m; ++i) {
        bmGs[i] = m;
    }
    j = 0;
    for (i = m - 1; i >= 0; --i) {
        if (i == -1 || suff[i] == i + 1) {
            for (; j < m - 1 - i; ++j) {
                if (bmGs[j] == m) {
                    bmGs[j] = m - 1 - i;
                }
            }
        }
    }
    for (i = 0; i <= m - 2; ++i) {
        bmGs[m - 1 - suff[i]] = m - 1 - i;
    }

    SCFree(suff);
}

/**
 * \brief Boyer Moore search algorithm
 *        Is better as the pattern length increases and for big buffers to search in.
 *        The algorithm needs a context of two arrays already prepared
 *        by prep_bad_chars() and prep_good_suffix()
 *
 * \param y pointer to the buffer to search in
 * \param n length limit of the buffer
 * \param x pointer to the pattern we ar searching for
 * \param m length limit of the needle
 * \param bmBc pointer to an array of BoyerMooreSuffixes prepared by prep_good_suffix()
 * \param bmGs pointer to an array of bachars prepared by prep_bad_chars()
 *
 * \retval ptr to start of the match; NULL if no match
 */
inline uint8_t *BoyerMoore(uint8_t *x, int32_t m, uint8_t *y, int32_t n, int32_t *bmGs, int32_t *bmBc) {
   int i, j, m1, m2;
#if 0
    printf("\nBad:\n");
    for (i=0;i<ALPHABET_SIZE;i++)
        printf("%c,%d ", i, bmBc[i]);

    printf("\ngood:\n");
    for (i=0;i<m;i++)
        printf("%c, %d ", x[i],bmBc[i]);
    printf("\n");
#endif
   j = 0;
   while (j <= n - m ) {
      for (i = m - 1; i >= 0 && x[i] == y[i + j]; --i);

      if (i < 0) {
         return y + j;
         j += bmGs[0];
      } else {
 //        printf("%c", y[i+j]);
         j += (m1 = bmGs[i]) > (m2 = bmBc[y[i + j]] - m + 1 + i)? m1: m2;
//            printf("%d, %d\n", m1, m2);
      }
   }
   return NULL;
}


/**
 * \brief Boyer Moore search algorithm
 *        Is better as the pattern length increases and for big buffers to search in.
 *        The algorithm needs a context of two arrays already prepared
 *        by prep_bad_chars() and prep_good_suffix()
 *
 * \param y pointer to the buffer to search in
 * \param n length limit of the buffer
 * \param x pointer to the pattern we ar searching for
 * \param m length limit of the needle
 * \param bmBc pointer to an array of BoyerMooreSuffixes prepared by prep_good_suffix()
 * \param bmGs pointer to an array of bachars prepared by prep_bad_chars()
 *
 * \retval ptr to start of the match; NULL if no match
 */
inline uint8_t *BoyerMooreNocase(uint8_t *x, int32_t m, uint8_t *y, int32_t n, int32_t *bmGs, int32_t *bmBc) {
    int i, j, m1, m2;
#if 0
    printf("\nBad:\n");
    for (i=0;i<ALPHABET_SIZE;i++)
        printf("%c,%d ", i, bmBc[i]);

    printf("\ngood:\n");
    for (i=0;i<m;i++)
        printf("%c, %d ", x[i],bmBc[i]);
    printf("\n");
#endif
    j = 0;
    while (j <= n - m ) {
        for (i = m - 1; i >= 0 && u8_tolower(x[i]) == u8_tolower(y[i + j]); --i);

        if (i < 0) {
            return y + j;
        } else {
            j += (m1=bmGs[i]) > (m2=bmBc[u8_tolower(y[i + j])] - m + 1 + i)?m1:m2;
        }
   }
   return NULL;
}

