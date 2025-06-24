import os
import sys
import math

def get_normals_buffer(vertices, normals, faces, normals_faces):
    # foreach vertex add the normal from "normals" at normalsfaces' index in the final array
    normals_buffer = [0.0] * len(vertices)

    for i in range(len(normals_faces)):
        v = faces[i]
        n = normals_faces[i]

        normals_buffer[3*v] = normals[3*n]
        normals_buffer[3*v + 1] = normals[3*n + 1]
        normals_buffer[3*v + 2] = normals[3*n + 2]

    return normals_buffer

def get_uvs_buffer(vertices, uvs, faces, uvs_faces):
    uvs_buffer = [0.0] * (len(vertices) // 3) * 2
    # uvs_buffer = []

    for i in range(len(uvs_faces)):
        v = faces[i]
        u = uvs_faces[i]
        # uvs_buffer.append(uvs[u])
        
        if uvs_buffer[2*v] == 0.0:
            uvs_buffer[2*v] = uvs[2*u]
            uvs_buffer[2*v + 1] = uvs[2*u + 1]

    return uvs_buffer

def get_faces(vertices, normals, uvs, obj_faces):
    faces_buffer = []
    for l in obj_faces:
        for f in l.split():
            fi = f.split("/")

            vi = int(fi[0]) - 1
            ni = int(fi[2]) - 1
            ui = int(fi[1]) - 1

            # print(fi)
            # print("vertex %d : %f %f %f" % (int(fi[0]) - 1, vertices[3*vi], vertices[3*vi + 1], vertices[3*vi + 2]))
            
            # add vertices
            faces_buffer.append( vertices[3*vi] )
            faces_buffer.append( vertices[3*vi + 1] )
            faces_buffer.append( vertices[3*vi + 2] )
            # add normals
            if fi[2] != "":
                faces_buffer.append( normals[3*ni] )
                faces_buffer.append( normals[3*ni + 1] )
                faces_buffer.append( normals[3*ni + 2] )
            else:
                faces_buffer.append(0)
                faces_buffer.append(0)
                faces_buffer.append(0)
            # add uvs
            if fi[1] != "":
                faces_buffer.append( uvs[2*ui] )
                faces_buffer.append( uvs[2*ui + 1] )
            else:
                faces_buffer.append(0)
                faces_buffer.append(0)

    return faces_buffer            
    
def save_model_array(define, vertices, uvs, normals, faces):
    define_lower = define.lower()
    
    # saving
    m_file = open("%s.hpp" % define_lower, "w")

    m_file.write("#ifndef %s\n" % define.upper())
    m_file.write("#define %s\n\n" % define.upper())
    m_file.write("#include <gl/gl.h>\n")
    m_file.write("#include <gl/glu.h>\n")
    m_file.write("#include <vector>\n\n")

    m_file.write("static size_t %s_vertices_len = %d;\n" % (define_lower, len(vertices)))
    m_file.write("static size_t %s_uvs_len = %d;\n" % (define_lower, len(uvs)))    
    m_file.write("static size_t %s_normals_len = %d;\n\n" % (define_lower, len(normals)))
    
    m_file.write("static size_t %s_indices_len = %d;\n" % (define_lower, len(faces)))

    # vertices
    m_file.write("static GLfloat %s_vertices[%d] = { \n" % (define_lower, len(vertices)))
    m_file.write("\t")
    for i in range(len(vertices)):
        v = vertices[i]
        if i == len(vertices) - 1:
            m_file.write("%f " % v)
        else:
            m_file.write("%f, " % v)

    m_file.write("\n};\n\n")

    # uvs
    if len(uvs) > 0:
        m_file.write("static GLfloat %s_uvs[%d] = { \n" % (define_lower, len(uvs)))
        m_file.write("\t")
        for i in range(len(uvs)):
            u = uvs[i]
            if u == len(uvs) - 1:
                m_file.write("%f " % u)
            else:
                m_file.write("%f, " % u)

        m_file.write("\n};\n\n")

    # normals
    if len(normals) > 0:
        m_file.write("static GLfloat %s_normals[%d] = { \n" % (define_lower, len(normals)))
        m_file.write("\t")
        for i in range(len(normals)):
            n = normals[i]
            if n == len(normals) - 1:
                m_file.write("%f " % n)
            else:
                m_file.write("%f, " % n)

        m_file.write("\n};\n\n")

    # faces
    m_file.write("static GLuint %s_indices[%d] = { \n" % (define_lower, len(faces)))
    m_file.write("\t")
    for i in range(len(faces)):
        f = faces[i]
        if i == len(faces) - 1:
            m_file.write("%d " % f)
        else:
            m_file.write("%d, " % f)

    m_file.write("\n};\n\n")
    
    m_file.write("#endif")
    m_file.close()

def save_model_faces(define, faces_buffer):
    define_lower = define.lower()

    upper = 8
    vertex_count = len(faces_buffer) // upper

    # header
    m_file = open("%s.hpp" % define_lower, "w")

    m_file.write("#ifndef %s\n" % define.upper())
    m_file.write("#define %s\n\n" % define.upper())
    m_file.write("#include <gl/gl.h>\n")
    m_file.write("#include <gl/glu.h>\n")
    m_file.write("#include <vector>\n\n")

    m_file.write("static size_t %s_vertices_len = %d;\n" % (define_lower, vertex_count))

    # faces
    m_file.write("static vertex %s_vertices[%d] = { \n" % (define_lower, vertex_count))

    m_file.write("\t")
    
    i = 0
    while i < len(faces_buffer):
        m_file.write(" { ")

        for j in range(upper):
            m_file.write(" %f," % faces_buffer[i + j])

        if i < len(faces_buffer) - upper:
            m_file.write(" } ,")
        else:
            m_file.write(" } ")
        
        i += upper

    m_file.write("\n};\n\n")
    m_file.write("#endif")
    m_file.close()

for filename in os.listdir(os.getcwd()):
    if not filename.lower().endswith(".obj"):
        continue

    vertices = []
    uvs = []
    normals = []

    obj_faces = []
    
    uvs_faces = []
    normals_faces = []
    
    file = open(filename, "r")
    for line in file:
        line = line.strip() # strip new lines

        # vertices
        if line.startswith("v "):
           line = line[2:]
           for v in line.split():
               vertices.append(float(v))
                
        # normals
        if line.startswith("vn "):
            line = line[2:]
            for n in line.split():
                normals.append(float(n))
        # uvs
        if line.startswith("vt "):
            line = line[2:]
            for u in line.split():
                uvs.append(float(u))

        # faces
        if line.startswith("f "):
            line = line[2:]
            obj_faces.append(line)


    faces_buffer = get_faces(vertices, normals, uvs, obj_faces)
    
    # normals_buffer = get_normals_buffer(vertices, normals, faces, normals_faces)
    # uvs_buffer = get_uvs_buffer(vertices, uvs, faces, uvs_faces)
    
    base, ext = os.path.splitext(filename)
    save_model_faces("m_" + base, faces_buffer)
