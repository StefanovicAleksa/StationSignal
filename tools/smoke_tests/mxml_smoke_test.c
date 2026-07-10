#include <stdio.h>
#include <stdlib.h>
#include "mxml.h"

/* Proves libmxml links and can parse a real SCL file, not just well-formed XML in the abstract. */

#define SAMPLE_SCL_PATH "/home/aleksa/code/ied_reporter/libiec61850/examples/server_example_goose/simpleIO_direct_control_goose.cid"

int main(void) {
    FILE* fp = fopen(SAMPLE_SCL_PATH, "r");
    if (!fp) {
        fprintf(stderr, "[ERROR] Could not open sample SCL file: %s\n", SAMPLE_SCL_PATH);
        return EXIT_FAILURE;
    }

    mxml_node_t* tree = mxmlLoadFile(NULL, fp, MXML_NO_CALLBACK);
    fclose(fp);

    if (!tree) {
        fprintf(stderr, "[ERROR] mxmlLoadFile failed to parse %s\n", SAMPLE_SCL_PATH);
        return EXIT_FAILURE;
    }

    mxml_node_t* root = mxmlGetFirstChild(tree);
    while (root && mxmlGetType(root) != MXML_ELEMENT) {
        root = mxmlGetNextSibling(root);
    }

    if (!root) {
        fprintf(stderr, "[ERROR] No root element found in parsed tree\n");
        mxmlDelete(tree);
        return EXIT_FAILURE;
    }

    int childCount = 0;
    mxml_node_t* child = mxmlGetFirstChild(root);
    while (child) {
        childCount++;
        child = mxmlGetNextSibling(child);
    }

    printf("[SMOKE TEST] Parsed root element: <%s>\n", mxmlGetElement(root));
    printf("[SMOKE TEST] Root child node count: %d\n", childCount);
    printf("[SMOKE TEST] mxml linkage and SCL parsing verified successfully.\n");

    mxmlDelete(tree);
    return EXIT_SUCCESS;
}
