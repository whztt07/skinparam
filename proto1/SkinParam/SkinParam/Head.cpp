/**
 * Head renderer
 */

#include "stdafx.h"

#include "Head.h"

using namespace Skin;
using namespace Utils;

Head::Head() : MeshRenderable(_T("model\\head.OBJ")) {
}

Head::~Head() {}

XMMATRIX Head::getWorldMatrix() const {
	XMMATRIX mtWorld = XMMatrixScaling(10.0f, 10.0f, 10.0f);
	mtWorld *= XMMatrixRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), (float)(Math::PI / 2));
	return mtWorld;
}