        /*
         * Build the shackle from separate side posts and a top arch instead of
         * a filled rectangle with a fake cutout. The cavity sits behind the
         * brass, while the body later occludes the embedded ends of both posts.
         */
        m3d_screen_prism(lock_cx - 7.0f, shackle_top + 4.0f,
                         lock_cx + 7.0f, lock_body_top + 1.0f,
                         5.365f, 5.00f, HEX(0x21160D), layout);
        m3d_screen_prism(shackle_l + 1.0f, shackle_top + 2.0f,
                         shackle_l + 5.0f, lock_body_top + 1.0f,
                         5.390f, 4.91f, HEX(0x4A270F), layout);
        m3d_screen_prism(shackle_r - 5.0f, shackle_top + 2.0f,
                         shackle_r - 1.0f, lock_body_top + 1.0f,
                         5.390f, 4.91f, HEX(0x4A270F), layout);
        m3d_screen_prism(shackle_l + 1.0f, shackle_top,
                         shackle_r - 1.0f, shackle_top + 5.5f,
                         5.400f, 4.93f, HEX(0x9C6826), layout);
        m3d_screen_prism(shackle_l + 2.0f, shackle_top + 0.6f,
                         shackle_l + 4.2f, lock_body_top + 0.2f,
                         5.420f, 4.98f, HEX(0xC7963F), layout);
        m3d_screen_prism(shackle_r - 4.2f, shackle_top + 0.6f,
                         shackle_r - 2.0f, lock_body_top + 0.2f,
                         5.420f, 4.98f, HEX(0x80501F), layout);
        m3d_screen_quad(shackle_l + 3.0f, shackle_top + 0.8f,
                        shackle_r - 3.0f, shackle_top + 1.7f,
                        5.425f, HEX(0xF8D78A), layout);
