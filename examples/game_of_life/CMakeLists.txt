# Special game of life example.

# This is all a little convoluted in camke.
# All we want to do run pixsmal with game_of_life.psm with the glider_gun.png image
# and then run game_of_life.psm on the output from that, and again on the output from that etc...
# 
# Supreme surprise: this ends up simulating Conway's game of life. After a successful build
# you should see a number game_of_life_<number>.png images. Flick though them and you should see a
# glider gun making... gliders!

# Loop to generate lots of iterations of game of life.
copy_files("copy_game_of_life_example" ${CMAKE_CURRENT_SOURCE_DIR}/*.psm ${CMAKE_CURRENT_BINARY_DIR})

set(game_of_life_frame_outputs)
set(game_of_life_frame_number 1)
while(game_of_life_frame_number LESS_EQUAL 199)
   list(APPEND game_of_life_frame_outputs game_of_life_${game_of_life_frame_number}.png)
   math(EXPR game_of_life_frame_number "${game_of_life_frame_number} + 1")
endwhile()

add_custom_command(
   OUTPUT game_of_life_final.png
   BYPRODUCTS ${game_of_life_frame_outputs}
   COMMAND gnine --logCommand --chain-times=199 --emit-frames=game_of_life.png game_of_life.psm ../../example_data/glider_gun.png game_of_life_final.png
   DEPENDS gnine game_of_life.psm
)

add_custom_target(example_game_of_life ALL DEPENDS copy_game_of_life_example game_of_life_final.png)
