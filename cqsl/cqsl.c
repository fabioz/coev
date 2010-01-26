/*
 * Copyright 2009 Apache Software Foundation 
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You
 * may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 * This file contains code originally written, among others, by:
 *   Gregory Trubetskoy (module methods, mod_python's _apache.c)
 *   Rob McCool (bits and pieces from apache2.2's server/util.c)
 *   Alexander Sabourenkov (putting it all together, kwargs support)
 *
 */
 
#include "Python.h"
#include <string.h>

#include "apr_lib.h"

#define AP_DECLARE(type) static type
#define OK 0
#define HTTP_BAD_REQUEST                   400
#define HTTP_NOT_FOUND                     404
#define IS_SLASH(s) (s == '/')


PyDoc_STRVAR(cqsl_doc,
"C implementations of cgi.parse_qs() and cgi.parse_qsl() (now in urlparse too),\n"
"lifted from mod_python, dependencies lifted from apache 2.2\n"
"\n"
"This implementation is about 5 times faster than python code, and this shows\n"
"in web applications that have long and windy query strings - parse_qsl()\n"
"in profiler's top prompted this module, and it brought a noticeable\n"
"increase in throughput\n"
"\n\nTypical usage pattern:\n\n"
"# somewhere in app's initialization ...\n"
"import cqsl, urlparse, cgi\n"
"\n"
"cgi.parse_qs = cqsl.parse_qs\n"
"cgi.parse_qsl = cqsl.parse_qsl\n"
"urlparse.parse_qs = cqsl.parse_qs\n"
"urlparse.parse_qsl = cqsl.parse_qsl\n"
);




static char x2c(const char *what)
{
    register char digit;

#if !APR_CHARSET_EBCDIC
    digit = ((what[0] >= 'A') ? ((what[0] & 0xdf) - 'A') + 10
             : (what[0] - '0'));
    digit *= 16;
    digit += (what[1] >= 'A' ? ((what[1] & 0xdf) - 'A') + 10
              : (what[1] - '0'));
#else /*APR_CHARSET_EBCDIC*/
    char xstr[5];
    xstr[0]='0';
    xstr[1]='x';
    xstr[2]=what[0];
    xstr[3]=what[1];
    xstr[4]='\0';
    digit = apr_xlate_conv_byte(ap_hdrs_from_ascii,
                                0xFF & strtol(xstr, NULL, 16));
#endif /*APR_CHARSET_EBCDIC*/
    return (digit);
}


/*
 * Unescapes a URL.
 * Returns 0 on success, non-zero on error
 * Failure is due to
 *   bad % escape       returns HTTP_BAD_REQUEST
 *
 *   decoding %00 -> \0  (the null character)
 *   decoding %2f -> /   (a special character)
 *                      returns HTTP_NOT_FOUND
 */
AP_DECLARE(int) ap_unescape_url(char *url)
{
    register int badesc, badpath;
    char *x, *y;

    badesc = 0;
    badpath = 0;
    /* Initial scan for first '%'. Don't bother writing values before
     * seeing a '%' */
    y = strchr(url, '%');
    if (y == NULL) {
        return OK;
    }
    for (x = y; *y; ++x, ++y) {
        if (*y != '%')
            *x = *y;
        else {
            if (!apr_isxdigit(*(y + 1)) || !apr_isxdigit(*(y + 2))) {
                badesc = 1;
                *x = '%';
            }
            else {
                *x = x2c(y + 1);
                y += 2;
                if (IS_SLASH(*x) || *x == '\0')
                    badpath = 1;
            }
        }
    }
    *x = '\0';
    if (badesc)
        return HTTP_BAD_REQUEST;
    else if (badpath)
        return HTTP_NOT_FOUND;
    else
        return OK;
}


/**
 ** parse_qs
 **
 *   This is a C version of cgi.parse_qs
 */
PyDoc_STRVAR(parse_qs_doc, "C implementation of cgi.parse_qs()");
static PyObject *parse_qs(PyObject *self, PyObject *args, PyObject *kw)
{

    PyObject *pairs, *dict;
    int i, n, len, lsize;
    char *qs;
    int keep_blank_values = 0;
    int strict_parsing = 0; /* XXX not implemented */
    char *keywords[] = { "qs", "keep_blank_values", "strict_parsing", 0 };
    
    if (! PyArg_ParseTupleAndKeywords(args, kw, "s|ii", keywords, &qs, &keep_blank_values, 
                           &strict_parsing)) 
        return NULL; /* error */

    /* split query string by '&' and ';' into a list of pairs */
    /* PYTHON 2.5: 'PyList_New' uses Py_ssize_t for input parameters */ 
    pairs = PyList_New(0);
    if (pairs == NULL)
        return NULL;

    i = 0;
    len = strlen(qs);

    while (i < len) {

        PyObject *pair;
        char *cpair;
        int j = 0;

        /* PYTHON 2.5: 'PyString_FromStringAndSize' uses Py_ssize_t for input parameters */ 
        pair = PyString_FromStringAndSize(NULL, len);
        if (pair == NULL)
            return NULL;

        /* split by '&' or ';' */
        cpair = PyString_AS_STRING(pair);
        while ((qs[i] != '&') && (qs[i] != ';') && (i < len)) {
            /* replace '+' with ' ' */
            cpair[j] = (qs[i] == '+') ? ' ' : qs[i];
            i++;
            j++;
        }

        if (j) {
            /* PYTHON 2.5: '_PyString_Resize' uses Py_ssize_t for input parameters */ 
            _PyString_Resize(&pair, j);
            if (pair)
                PyList_Append(pairs, pair);
        }

        Py_XDECREF(pair);
        i++;
    }

    /*
     * now we have a list of "abc=def" string (pairs), let's split 
     * them all by '=' and put them in a dictionary.
     */
    
    dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    /* PYTHON 2.5: 'PyList_Size' uses Py_ssize_t for input parameters */ 
    lsize = PyList_Size(pairs);
    n = 0;

    while (n < lsize) {

        PyObject *pair, *key, *val;
        char *cpair, *ckey, *cval;
        int k, v;

        pair = PyList_GET_ITEM(pairs, n);
        cpair = PyString_AS_STRING(pair);

        len = strlen(cpair);
        /* PYTHON 2.5: 'PyString_FromStringAndSize' uses Py_ssize_t for input parameters */ 
        key = PyString_FromStringAndSize(NULL, len);
        if (key == NULL) 
            return NULL;
        /* PYTHON 2.5: 'PyString_FromStringAndSize' uses Py_ssize_t for input parameters */ 
        val = PyString_FromStringAndSize(NULL, len);
        if (val == NULL) 
            return NULL;

        ckey = PyString_AS_STRING(key);
        cval = PyString_AS_STRING(val);

        i = 0;
        k = 0;
        v = 0;
        while (i < len) {
            if (cpair[i] != '=') {
                ckey[k] = cpair[i];
                k++;
                i++;
            }
            else {
                i++;      /* skip '=' */
                while (i < len) {
                    cval[v] = cpair[i];
                    v++;
                    i++;
                }
            }
        }

        ckey[k] = '\0';
        cval[v] = '\0';

        if (keep_blank_values || (v > 0)) {

            ap_unescape_url(ckey);
            ap_unescape_url(cval);

            /* PYTHON 2.5: '_PyString_Resize' uses Py_ssize_t for input parameters */ 
            _PyString_Resize(&key, strlen(ckey));
            /* PYTHON 2.5: '_PyString_Resize' uses Py_ssize_t for input parameters */ 
            _PyString_Resize(&val, strlen(cval));

            if (key && val) {

                ckey = PyString_AS_STRING(key);
                cval = PyString_AS_STRING(val);
        
                if (PyMapping_HasKeyString(dict, ckey)) {
                    PyObject *list;
                    list = PyDict_GetItem(dict, key);
                    PyList_Append(list, val);
                    /* PyDict_GetItem is a borrowed ref, no decref */
                }
                else {
                    PyObject *list;
                    list = Py_BuildValue("[O]", val);
                    PyDict_SetItem(dict, key, list);
                    Py_DECREF(list);
                }
            }
        }

        Py_XDECREF(key);
        Py_XDECREF(val);

        n++;
    }

    Py_DECREF(pairs);
    return dict;
}

/**
 ** parse_qsl
 **
 *   This is a C version of cgi.parse_qsl
 */
PyDoc_STRVAR(parse_qsl_doc, "C implementation of cgi.parse_qsl()");
static PyObject *parse_qsl(PyObject *self, PyObject *args, PyObject *kw)
{

    PyObject *pairs;
    int i, len;
    char *qs;
    int keep_blank_values = 0;
    int strict_parsing = 0; /* XXX not implemented */
    char *keywords[] = { "qs", "keep_blank_values", "strict_parsing", 0 };
    
    if (! PyArg_ParseTupleAndKeywords(args, kw, "s|ii", keywords, &qs, &keep_blank_values, 
                           &strict_parsing)) 
        return NULL; /* error */

    /* split query string by '&' and ';' into a list of pairs */
    /* PYTHON 2.5: 'PyList_New' uses Py_ssize_t for input parameters */ 
    pairs = PyList_New(0);
    if (pairs == NULL)
        return NULL;

    i = 0;
    len = strlen(qs);

    while (i < len) {

        PyObject *pair, *key, *val;
        char *cpair, *ckey, *cval;
        int plen, j, p, k, v;

        /* PYTHON 2.5: 'PyString_FromStringAndSize' uses Py_ssize_t for input parameters */ 
        pair = PyString_FromStringAndSize(NULL, len);
        if (pair == NULL)
            return NULL;

        /* split by '&' or ';' */
        cpair = PyString_AS_STRING(pair);
        j = 0;
        while ((qs[i] != '&') && (qs[i] != ';') && (i < len)) {
            /* replace '+' with ' ' */
            cpair[j] = (qs[i] == '+') ? ' ' : qs[i];
            i++;
            j++;
        }

        if (j == 0) {
            Py_XDECREF(pair);
            i++;
            continue;
        }

        cpair[j] = '\0';
        /* PYTHON 2.5: '_PyString_Resize' uses Py_ssize_t for input parameters */ 
        _PyString_Resize(&pair, j);
        cpair = PyString_AS_STRING(pair);

        /* split the "abc=def" pair */
        plen = strlen(cpair);
        /* PYTHON 2.5: 'PyString_FromStringAndSize' uses Py_ssize_t for input parameters */ 
        key = PyString_FromStringAndSize(NULL, plen);
        if (key == NULL) 
            return NULL;
        /* PYTHON 2.5: 'PyString_FromStringAndSize' uses Py_ssize_t for input parameters */ 
        val = PyString_FromStringAndSize(NULL, plen);
        if (val == NULL) 
            return NULL;

        ckey = PyString_AS_STRING(key);
        cval = PyString_AS_STRING(val);

        p = 0;
        k = 0;
        v = 0;
        while (p < plen) {
            if (cpair[p] != '=') {
                ckey[k] = cpair[p];
                k++;
                p++;
            }
            else {
                p++;      /* skip '=' */
                while (p < plen) {
                    cval[v] = cpair[p];
                    v++;
                    p++;
                }
            }
        }
        ckey[k] = '\0';
        cval[v] = '\0';

        if (keep_blank_values || (v > 0)) {

            ap_unescape_url(ckey);
            ap_unescape_url(cval);

            /* PYTHON 2.5: '_PyString_Resize' uses Py_ssize_t for input parameters */
            _PyString_Resize(&key, strlen(ckey));
            /* PYTHON 2.5: '_PyString_Resize' uses Py_ssize_t for input parameters */
            _PyString_Resize(&val, strlen(cval));

            if (key && val) {
                PyObject* listitem = Py_BuildValue("(O,O)", key, val);
                if(listitem) {
                    PyList_Append(pairs, listitem);
                    Py_DECREF(listitem);
                }
            }

        }
        Py_XDECREF(pair);
        Py_XDECREF(key);
        Py_XDECREF(val);
        i++;
    }

    return pairs;
}

static PyMethodDef cqsl_methods[] = {
    { "parse_qs", (PyCFunction) parse_qs, METH_KEYWORDS, parse_qs_doc },
    { "parse_qsl", (PyCFunction) parse_qsl, METH_KEYWORDS, parse_qsl_doc },
    { 0 },
};

PyMODINIT_FUNC
initcqsl(void) {
    PyObject *m;
    
    m = Py_InitModule3("cqsl", cqsl_methods, cqsl_doc);
    if (m == NULL)
	return;
    
    PyModule_AddStringConstant(m, "__version__", "1.0");
}
