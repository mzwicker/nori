/*
    This file is part of Nori, a simple educational ray tracer

    Copyright (c) 2015 by Wenzel Jakob

    Nori is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Nori is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <nori/parser.h>
#include <nori/scene.h>
#include <nori/camera.h>
#include <nori/block.h>
#include <nori/timer.h>
#include <nori/bitmap.h>
#include <nori/sampler.h>
#include <nori/integrator.h>
#include <nori/gui.h>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <filesystem/resolver.h>
#include <thread>

using namespace nori;

static void renderBlock(const Scene *scene, Sampler *sampler, ImageBlock &block) {
    const Camera *camera = scene->getCamera();
    const Integrator *integrator = scene->getIntegrator();

    Point2i offset = block.getOffset();
    Vector2i size  = block.getSize();

    /* Clear the block contents */
    block.clear();

    /* For each pixel and pixel sample sample */
    for (int y=0; y<size.y(); ++y) {
        for (int x=0; x<size.x(); ++x) {
            for (uint32_t i=0; i<sampler->getSampleCount(); ++i) {
                Point2f pixelSample = Point2f((float) (x + offset.x()), (float) (y + offset.y())) + sampler->next2D();
                Point2f apertureSample = sampler->next2D();

                /* Sample a ray from the camera */
                Ray3f ray;
                Color3f value = camera->sampleRay(ray, pixelSample, apertureSample);

                /* Compute the incident radiance */
                value *= integrator->Li(scene, sampler, ray);

                /* Store in the image block */
                block.put(pixelSample, value);
            }
        }
    }
}

static void render(Scene *scene, const std::string &filename) {
    const Camera *camera = scene->getCamera();
    Vector2i outputSize = camera->getOutputSize();
    scene->getIntegrator()->preprocess(scene);

    /* Create a block generator (i.e. a work scheduler) */
    BlockGenerator blockGenerator(outputSize, NORI_BLOCK_SIZE);

    /* Allocate memory for the entire output image and clear it */
    ImageBlock result(outputSize, camera->getReconstructionFilter());
    result.clear();

#if NO_GUI==0
    /* Create a window that visualizes the partially rendered result */
    nanogui::init();
    NoriScreen *screen = new NoriScreen(result);
#endif

    /* Do the following in parallel and asynchronously */
    std::thread render_thread([&] {
        cout << "Rendering .. ";
        cout.flush();
        Timer timer;

        tbb::blocked_range<int> range(0, blockGenerator.getBlockCount());

        auto map = [&](const tbb::blocked_range<int> &range) {
            /* Allocate memory for a small image block to be rendered
               by the current thread */
            ImageBlock block(Vector2i(NORI_BLOCK_SIZE),
                camera->getReconstructionFilter());

            /* Create a clone of the sampler for the current thread */
            std::unique_ptr<Sampler> sampler(scene->getSampler()->clone());

            for (int i=range.begin(); i<range.end(); ++i) {
                /* Request an image block from the block generator */
                blockGenerator.next(block);

                /* Inform the sampler about the block to be rendered */
                sampler->prepare(block);

                /* Render all contained pixels */
                renderBlock(scene, sampler.get(), block);

                /* The image block has been processed. Now add it to
                   the "big" block that represents the entire image */
                result.put(block);
            }
        };

        /// Uncomment the following line for single threaded rendering
        // map(range);

        /// Default: parallel rendering
        tbb::parallel_for(range, map);

        cout << "done. (took " << timer.elapsedString() << ")" << endl;
    });

#if NO_GUI==0
    /* Enter the application main loop */
    nanogui::mainloop();
#endif

    /* Shut down the user interface */
    render_thread.join();

#if NO_GUI==0
    delete screen;
    nanogui::shutdown();
#endif

    /* Now turn the rendered image block into
       a properly normalized bitmap */
    std::unique_ptr<Bitmap> bitmap(result.toBitmap());

    /* Determine the filename of the output bitmap */
    std::string outputName = filename;
    size_t lastdot = outputName.find_last_of(".");
    if (lastdot != std::string::npos)
        outputName.erase(lastdot, std::string::npos);
    outputName += ".exr";

    /* Save using the OpenEXR format */
    bitmap->save(outputName);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        cerr << "Syntax: " << argv[0] << " <scene.xml>" << endl;
        return -1;
    }

    filesystem::path path(argv[1]);

    try {
        if (path.extension() == "xml") {
            /* Add the parent directory of the scene file to the
               file resolver. That way, the XML file can reference
               resources (OBJ files, textures) using relative paths */
            getFileResolver()->prepend(path.parent_path());

            std::unique_ptr<NoriObject> root(loadFromXML(argv[1]));

            /* When the XML root object is a scene, start rendering it .. */
            if (root->getClassType() == NoriObject::EScene)
                render(static_cast<Scene *>(root.get()), argv[1]);
        } else if (path.extension() == "exr") {
            /* Alternatively, provide a basic OpenEXR image viewer */
            Bitmap bitmap(argv[1]);
            ImageBlock block(Vector2i((int) bitmap.cols(), (int) bitmap.rows()), nullptr);
            block.fromBitmap(bitmap);
            nanogui::init();
            NoriScreen *screen = new NoriScreen(block);
            nanogui::mainloop();
            delete screen;
            nanogui::shutdown();
        } else {
            cerr << "Fatal error: unknown file \"" << argv[1]
                 << "\", expected an extension of type .xml or .exr" << endl;
        }
    } catch (const std::exception &e) {
        cerr << "Fatal error: " << e.what() << endl;
        return -1;
    }
    return 0;
}
