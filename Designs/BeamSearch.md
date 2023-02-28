This is the design for the beam search decoding algorithm.

ContextImpl.cpp is where greedy decoding is implemented.

sampleBest() does the following:

1. Populate `probs_ids` with (probability, idx).
2. Find the `top_k` most likely tokens.
3. Filter out tokens matching `token_sot`, `token_solm`, `token_not`.
   3.1. sot == start of text, solm = ???, not == not timestamps
4. Return the result.

We should modify this to return the most likely N tokens. We'll call this
sampleBestN().

Greedy search works like this:

1. Call decode(prompt, tokens).
   1.1. This populates `probs`. Need to wrap this in a vector of size `N_BEAMS`.
   1.2. Need to wrap `prompt` in a vector of size `N_BEAMS`.
2. Extract the most likely token and add it to the prompt.
3. Repeat until EOT (end of transcript) or max tokens or end of audio stream.

Beam search will work like this:

1. Initialize `prompts` as a vector containing `prompt`.
2. Call decode(prompt, tokens) for each prompt in `prompts`.
3. Extract the most likely `N_BEAMS` tokens in each result.
4. Compute joint probabilities for each token.
5. Extract the most likely `N_BEAMS` prompts using joint probabilities.
6. Update `prompts`.
7. Repeat until end condition is reached for all prompts.
8. Return most likely prompt.
