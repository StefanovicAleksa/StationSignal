#!/bin/bash

# --- Show Usage ---
show_usage() {
    echo "Usage: $0 [options] folder1 [folder2 ...]"
    echo ""
    echo "Combines all non-ignored files (per .gitignore) from specified folders"
    echo "into a single output file. MUST be run from within a git repository."
    echo ""
    echo "Options:"
    echo "  -o, --output FILE    Output file name (default: CombinedOutput.txt)"
    echo "  -i, --ignore PATTERN Additional file or pattern to ignore (can be used multiple times)"
    echo "  -v, --verbose        Enable verbose output"
    echo "  -h, --help           Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 ./main ./test"
    echo "  $0 -o MyProject.txt ./main"
    echo "  $0 -i \"*.md\" -i \"secret_config.json\" ./main"
}

# --- Initialize Variables ---
declare -a folder_paths=()
declare -a ignore_patterns=()
output_file="CombinedOutput.txt"
verbose=false

# --- Parse Arguments (Robust Flag-Based Parsing) ---
while [[ $# -gt 0 ]]; do
    case $1 in
        -o|--output)
            if [[ -n "$2" ]]; then
                output_file="$2"
                shift 2
            else
                echo "Error: --output requires an argument."
                exit 1
            fi
            ;;
        -i|--ignore)
            if [[ -n "$2" ]]; then
                ignore_patterns+=("$2")
                shift 2
            else
                echo "Error: --ignore requires an argument."
                exit 1
            fi
            ;;
        -v|--verbose)
            verbose=true
            shift
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        -*)
            echo "Error: Unknown option: $1"
            show_usage
            exit 1
            ;;
        *)
            # This is not an option, so it must be a folder
            if [[ -d "$1" ]]; then
                folder_paths+=("$1")
                shift
            else
                echo "Error: '$1' is not a directory."
                show_usage
                exit 1
            fi
            ;;
    esac
done

# --- Process a Folder ---
process_folder() {
    local folder_path="$1"
    local temp_file="$2"

    if [[ "$verbose" == "true" ]]; then
        echo "Processing folder: $folder_path"
    fi

    # Add folder header
    echo "# ===== Files from: $folder_path =====" >> "$temp_file"
    echo "" >> "$temp_file"

    processed_count=0

    pushd "$folder_path" > /dev/null

    while IFS= read -r file; do
        if [[ -f "$file" ]]; then

            # Check against bonus ignore patterns
            local skip_file=false
            for pattern in "${ignore_patterns[@]}"; do
                # Use case statement for glob matching support
                case "$file" in
                    $pattern)
                        skip_file=true
                        break
                        ;;
                esac
            done

            if [[ "$skip_file" == true ]]; then
                if [[ "$verbose" == "true" ]]; then
                    echo "  Skipped by --ignore flag: $file"
                fi
                continue
            fi

            # Add file to output
            echo "# File: $file" >> "$temp_file"
            cat "$file" >> "$temp_file"
            echo "" >> "$temp_file"

            ((processed_count++))
            if [[ "$verbose" == "true" ]]; then
                echo "  Added: $file"
            fi
        fi
    done < <(git ls-files --cached --others --exclude-standard .)

    # Return to the original directory
    popd > /dev/null

    # Add folder footer
    echo "# ===== End of files from: $folder_path =====" >> "$temp_file"
    echo "" >> "$temp_file"

    echo "Processed $processed_count non-ignored files from $folder_path."
}

# --- Main Function ---
main() {
    # Check if we are in a git repository
    if ! git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
        echo "Error: This script must be run from within a git repository."
        exit 1
    fi

    # Check if at least one folder was provided
    if [[ ${#folder_paths[@]} -eq 0 ]]; then
        echo "Error: At least one folder path is required."
        show_usage
        exit 1
    fi

    # Create a temporary file
    temp_file=$(mktemp)

    # Process each folder
    for folder in "${folder_paths[@]}"; do
        process_folder "$folder" "$temp_file"
    done

    # Move temp file to final output file
    mv "$temp_file" "$output_file"

    echo ""
    echo "✅ Success! All non-ignored files combined into: $output_file"
}

# --- Run Main ---
main