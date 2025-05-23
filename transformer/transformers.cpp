#define PROFILE
#include "openfhe/pke/openfhe.h"
#include <cmath>
#include <array>
using namespace lbcrypto;
using namespace std;
using Vector = vector<double>; 
using EmbeddingMatrix = vector<Vector>;
using DotProdMatrix = vector<vector<Ciphertext<DCRTPoly>>>;


// Adds sinusoidal positional encoding to token embeddings
EmbeddingMatrix addPositionalEncoding(const EmbeddingMatrix& embeddings){
    EmbeddingMatrix peMatrix;
    size_t words = embeddings.size();
    size_t dim = embeddings[0].size();

    for (int i = 0; i < words; i++){
        vector<double> wordPE = {};
        for (int j = 0; j < dim; j++)
        {
            if (j % 2 == 0)
            {
                wordPE.push_back(sin((i) / pow((10000), (j/dim))));

            }
            else{
                wordPE.push_back(cos((i) / pow((10000), (j/dim))));
            }
        }
        peMatrix.push_back(wordPE);
    }

    for (size_t i = 0; i < words; i++) {
        for (size_t j = 0; j < dim; j++) {
            peMatrix[i][j] += embeddings[i][j];
        }
    }

    return peMatrix;
}

// Calculates the k-th diagonal of a matrix
vector<double> calculateDiagonal(const EmbeddingMatrix& mat, int diagNum){
    size_t size = mat.size();
    vector<double> diagonalMat;
    for (size_t i = 0; i < size; i++) {
        diagonalMat.push_back(mat[i][(i + diagNum) % size]);
    }
    return diagonalMat;
}

// Applies diagonal encoding based matrix-vector multiplication
array<Ciphertext<DCRTPoly>, 3> applyDiagonalProjection(const vector<Ciphertext<DCRTPoly>>& encPE,
                                                       const EmbeddingMatrix& W_,
                                                       CryptoContext<DCRTPoly> cc) {
    const size_t words = 3;
    const size_t dim = 4;
    array<Ciphertext<DCRTPoly>, words> p;

    for (size_t i = 0; i < words; i++) {
        const auto& encTok = encPE[i];
        for (int j = 0; j < dim; j++){
            auto product = cc -> EvalMult((cc -> EvalRotate(encTok, j)), cc-> MakeCKKSPackedPlaintext(calculateDiagonal(W_, j)));
            p[i] = (j==0) ? product : cc -> EvalAdd(p[i], product) ;
        }
    }
    return p;
}



// Computes dot product between two ciphertexts
Ciphertext<DCRTPoly> evalDotProduct(const Ciphertext<DCRTPoly>& q,
                                    const Ciphertext<DCRTPoly>& k,
                                    CryptoContext<DCRTPoly> cc,
                                    size_t dim) {
    return cc->EvalSum(cc->EvalMult(q, k), dim);
}

// Performs attention-weighted sum of values
void evalOutput(const vector<vector<Ciphertext<DCRTPoly>>>& score,
                const array<Ciphertext<DCRTPoly>, 3>& v,
                vector<Ciphertext<DCRTPoly>>* output,
                CryptoContext<DCRTPoly> cc) {
    output->resize(3);
    for (size_t i = 0; i < 3; i++) {
        for (size_t j = 0; j < 3; j++) {
            auto weighted = cc->EvalMult(score[i][j], v[j]);
            (*output)[i] = (j == 0) ? weighted : cc->EvalAdd((*output)[i], weighted);
        }
    }
}

// Adds residual connection to output
void evalOutputWithResidual(vector<Ciphertext<DCRTPoly>>* output,
                            const vector<Ciphertext<DCRTPoly>>& encPE,
                            CryptoContext<DCRTPoly> cc) {
    for (size_t i = 0; i < 3; i++) {
        (*output)[i] = cc->EvalAdd(encPE[i], (*output)[i]);
    }
}

// Applies feedforward network (x*W1)^2 * W2
void evalFeedForward(vector<Ciphertext<DCRTPoly>>* output,
                     const vector<double>& w1,
                     const vector<double>& w2,
                     CryptoContext<DCRTPoly> cc) {
    Plaintext ptxtW1 = cc->MakeCKKSPackedPlaintext(w1);
    Plaintext ptxtW2 = cc->MakeCKKSPackedPlaintext(w2);

    for (size_t i = 0; i < 3; i++) {
        (*output)[i] = cc->EvalMult((*output)[i], ptxtW1);
        (*output)[i] = cc->EvalMult((*output)[i], (*output)[i]);
        (*output)[i] = cc->EvalMult((*output)[i], ptxtW2);
    }
} 

int main() {
    EmbeddingMatrix embeddings = {
        {0.1, 0.3, 0.2, 0.05},  // "the"
        {0.4, 0.1, 0.2, 0.3},   // "cat"
        {0.3, 0.4, 0.1, 0.2}    // "sat"
    };

    uint32_t scaleModSize = 50;
    uint32_t multDepth = 6;
    uint32_t batchSize = 4;

    CCParams<CryptoContextCKKSRNS> parameters;
    parameters.SetScalingModSize(scaleModSize);
    parameters.SetMultiplicativeDepth(multDepth);
    parameters.SetBatchSize(batchSize);

    CryptoContext<DCRTPoly> cc = GenCryptoContext(parameters);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);
    cc->Enable(ADVANCEDSHE);

    size_t words = embeddings.size();
    size_t dim = embeddings[0].size();

    EmbeddingMatrix peMatrix = addPositionalEncoding(embeddings);

    auto keys = cc->KeyGen();
    cc->EvalMultKeyGen(keys.secretKey);

    vector<int32_t> rotIndices;
    for (size_t i = 0; i < dim; i++) rotIndices.push_back(i);
    cc->EvalAtIndexKeyGen(keys.secretKey, rotIndices);

    vector<Ciphertext<DCRTPoly>> encPE;
    for (size_t i = 0; i < words; i++) {
        Plaintext ptxt = cc->MakeCKKSPackedPlaintext(peMatrix[i]);
        encPE.push_back(cc->Encrypt(keys.publicKey, ptxt));
    }

    EmbeddingMatrix W_Q = {{0.1, 0.2, 0.3, 0.4}, {0.5, 0.6, 0.7, 0.8}, {0.9, 1.0, 1.1, 1.2}, {1.3, 1.4, 1.5, 1.6}};
    EmbeddingMatrix W_K = {{0.2, 0.1, 0.4, 0.3}, {0.6, 0.5, 0.8, 0.7}, {1.0, 0.9, 1.2, 1.1}, {1.4, 1.3, 1.6, 1.5}};
    EmbeddingMatrix W_V = {{0.3, 0.4, 0.1, 0.2}, {0.7, 0.8, 0.5, 0.6}, {1.1, 1.2, 0.9, 1.0}, {1.5, 1.6, 1.3, 1.4}};

    auto q = applyDiagonalProjection(encPE, W_Q, cc);
    auto k = applyDiagonalProjection(encPE, W_K, cc);
    auto v = applyDiagonalProjection(encPE, W_V, cc);

    DotProdMatrix score(words, vector<Ciphertext<DCRTPoly>>(words));
    for (size_t i = 0; i < words; i++) {
        for (size_t j = 0; j < words; j++) {
            score[i][j] = evalDotProduct(q[i], k[j], cc, dim);
        }
    }

    vector<Ciphertext<DCRTPoly>> output;
    evalOutput(score, v, &output, cc);
    evalOutputWithResidual(&output, encPE, cc);

    vector<double> w1 = {0.3, 0.7, 0.2, 0.5};
    vector<double> w2 = {0.6, 0.4, 0.8, 0.1};
    evalFeedForward(&output, w1, w2, cc);

    for (size_t i = 0; i < words; i++) {
        Plaintext decrypted;
        cc->Decrypt(keys.secretKey, output[i], &decrypted);
        decrypted->SetLength(dim);

        cout << decrypted->GetRealPackedValue() << endl;
    }
}