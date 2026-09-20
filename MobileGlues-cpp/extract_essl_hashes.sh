#!/data/data/com.termux/files/usr/bin/bash
# Extrai os hashes dos ESSL existentes
cd /storage/emulated/0/Ltw/cache/ESSL

echo "hash_essl,nome_arquivo,tamanho,primeiras_linhas"
for f in *.essl; do
    hash=$(basename "$f" .essl)
    size=$(wc -c < "$f")
    lines=$(head -1 "$f")
    echo "$hash,$f,$size,$lines"
done
