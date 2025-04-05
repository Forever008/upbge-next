void main(void)
{
  int v = gl_VertexID % 3;
  float x = -1.0 + float((v & 1) << 2);
  float y = -1.0 + float((v & 2) << 1);
  gl_Position = vec4(x, y, 1.0, 1.0);
  bgl_TexCoord = vec4((gl_Position.xy + 1.0) * 0.5, 0.0, 0.0);
}
