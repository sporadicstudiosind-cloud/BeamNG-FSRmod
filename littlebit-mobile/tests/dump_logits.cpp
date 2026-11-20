// dump_logits: feed a list of token ids one at a time and write the logits after each token
// usage: dump_logits model.gguf out.f32 tok0 tok1 ...
// output: raw float32, n_tokens x n_vocab, row-major
#include "llama.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

int main(int argc, char ** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s model.gguf out.f32 tok0 [tok1 ...]\n", argv[0]);
        return 1;
    }
    std::vector<llama_token> toks;
    for (int i = 3; i < argc; ++i) {
        toks.push_back((llama_token) atoi(argv[i]));
    }

    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    llama_model * model = llama_model_load_from_file(argv[1], mp);
    if (!model) {
        fprintf(stderr, "failed to load model\n");
        return 1;
    }
    const int n_vocab = llama_vocab_n_tokens(llama_model_get_vocab(model));

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx   = 512;
    cp.n_batch = 512;
    llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        fprintf(stderr, "failed to create context\n");
        return 1;
    }

    FILE * f = fopen(argv[2], "wb");
    for (size_t i = 0; i < toks.size(); ++i) {
        llama_batch b = llama_batch_get_one(&toks[i], 1);
        if (llama_decode(ctx, b) != 0) {
            fprintf(stderr, "decode failed at %zu\n", i);
            return 1;
        }
        const float * logits = llama_get_logits_ith(ctx, -1);
        fwrite(logits, sizeof(float), n_vocab, f);
    }
    fclose(f);
    fprintf(stderr, "wrote %zu x %d logits\n", toks.size(), n_vocab);

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
