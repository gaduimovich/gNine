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

set(iterate_frame_outputs)
set(iterate_number 1)
while(iterate_number LESS_EQUAL 199)
   list(APPEND iterate_frame_outputs game_of_life_iterate_${iterate_number}.png)
   math(EXPR iterate_number "${iterate_number} + 1")
endwhile()

set(iter_transition_frame_outputs)
set(iter_transition_number 1)
while(iter_transition_number LESS_EQUAL 199)
   list(APPEND iter_transition_frame_outputs game_of_life_iter_transition_${iter_transition_number}.png)
   math(EXPR iter_transition_number "${iter_transition_number} + 1")
endwhile()

add_custom_command(
   OUTPUT game_of_life_iterate_final.png
   BYPRODUCTS ${iterate_frame_outputs}
   COMMAND gnine --logCommand --emit-frames=game_of_life_iterate.png game_of_life_iterate.psm ../../example_data/glider_gun.png game_of_life_iterate_final.png
   DEPENDS gnine game_of_life_iterate.psm
)

add_custom_command(
   OUTPUT game_of_life_iter_transition_final.png
   BYPRODUCTS ${iter_transition_frame_outputs}
   COMMAND gnine --logCommand --emit-frames=game_of_life_iter_transition.png game_of_life_iter_transition.psm ../../example_data/glider_gun.png game_of_life_iter_transition_final.png
   DEPENDS gnine game_of_life_iter_transition.psm
)

add_custom_target(example_game_of_life ALL DEPENDS copy_game_of_life_example game_of_life_iterate_final.png game_of_life_iter_transition_final.png)
