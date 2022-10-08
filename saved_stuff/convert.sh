#!/bin/bash

layers=(clear paint_bucket squarewave polygon ellipse rect_filled rect circle_filled circle curve line square_pen circle_pen)

echo ${layers[@]}

echo ${#layers[@]}

DEST="$1"

for (( i=0; i<${#layers[@]}; i++ )); do
    cp "$(printf "data/%03d.png" $(( $i * 2 )) )" ${DEST}/${layers[$i]}_active.png
    cp "$(printf "data/%03d.png" $(( $i * 2 + 1 )) )" ${DEST}/${layers[$i]}_inactive.png
done
