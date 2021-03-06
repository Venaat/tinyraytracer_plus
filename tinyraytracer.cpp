#define _USE_MATH_DEFINES
#include <cmath>
#include <limits>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "model.h"
#include "geometry.h"

int envmap_width, envmap_height;
std::vector<Vec3f> envmap;



struct Light {
    Light(const Vec3f &p, const float i) : position(p), intensity(i) {}
    Vec3f position;
    float intensity;
};

struct Material {
    Material(const float r, const Vec4f &a, const Vec3f &color, const float spec) : refractive_index(r), albedo(a), diffuse_color(color), specular_exponent(spec) {}
    Material() : refractive_index(1), albedo(1,0,0,0), diffuse_color(), specular_exponent() {}
    float refractive_index;
    Vec4f albedo;
    Vec3f diffuse_color;
    float specular_exponent;
};

struct Sphere {
    Vec3f center;
    float radius;
    Material material;
    
    Sphere(const Vec3f &c, const float r, const Material &m) : center(c), radius(r), material(m) {}

    bool ray_intersect(const Vec3f &orig, const Vec3f &dir, float &t0) const {
        Vec3f L = center - orig;                     // L = vecteur origine du rayon vers centre du cercle 
        float tca = L*dir;                                // Projection de L sur le rayon, ce qui donne la longueur de tca
        float d2 = L*L - tca*tca;                     // On calcule d avec pythagore, qui est la projection du centre du cercle sur le rayon
                                                                 // pour préparer le calcul de thc et faire la vérification de la ligne suivante 
                                                                 // on n'utilise pas encore le sqrt parce que pas nécessaire + temps gagné
                                                                  
        if (d2 > radius*radius) return false;  // si d est supérieur au rayon du cercle, i.e si d2 est supérieur au rayon du cercle ^2
                                                                // alors le rayon passe au-dessus de la sphère, donc pas d'intersection
                                                                
        float thc = sqrtf(radius*radius - d2); // on calcule thc avec pythagore puisqu'on a d maintenant 
        t0       = tca - thc;                             // on calcule t0, le premier point d'intersection de la sphère avec le rayon 
        float t1 = tca + thc;                          // on calcule t1, le deuxième point d'intersection de la sphère avec le rayon
        if (t0 < 0) t0 = t1;                             // si le premier point d'intersection est derrière l'origine, on l'oublie et on check le 2e
        if (t0 < 0) return false;                     // si le deuxième aussi est derrière, c'est qu'il n'y a pas d'intersection finalement 
        return true;                                      // on renvoie vrai et t0 = la distance de la 1ere intersection valide avec la sphère
    }
    
    // On renvoie les deux distances d'intersection au lieu de juste la 1ere ici 
    bool ray_intersect(const Vec3f &orig, const Vec3f &dir, float &t0, float &t1) const { 
        Vec3f L = center - orig;
        float tca = L*dir;
        float d2 = L*L - tca*tca ;
        
        if (d2 > radius*radius) return false;
        
        float thc = sqrt(radius*radius - d2);
        t0 = tca - thc ;
        t1 = tca + thc;
        
        if (t0 < 0 && t1 < 0) return false;  // si les deux intersections du rayon sont avant son origine, c'est qu'il n'y a pas d'intersection
        return true;
    }
};

struct DeathSphere {
    Sphere sphereA;
    Sphere sphereB;
    
    DeathSphere(Sphere sa, Sphere sb) :  sphereA(sa), sphereB(sb) {}
    
    bool ray_intersect(const Vec3f &orig, const Vec3f &dir, float &t0) const {
        // Il faut que le rayon intersecte les deux sphères pour qu'il y ait un calcul supplémentaire à faire 
        float t0a, t1a, t0b, t1b;
        
        // Si on n'intersecte même pas la sphère A, ça ne sert à rien de continuer 
        if (! sphereA.ray_intersect(orig, dir, t0a, t1a)) return false;
        
        // S'il y a intersection mais que le 1er point est derrière l'origine, on prend le 2e
        if (t0a < 0) t0a = t1a;
        
        // Si on intersecte les deux sphères, on doit trouver le tb qui est entre t0a et t1a
        if (sphereB.ray_intersect(orig, dir, t0b, t1b)){
             // Cas 1 et 2
            (t0b > t0a) ? t0 = t0b : t0 = t1b;
            
            // Voir Cas 3 Schéma, "faux" passage entre les 2 sphères
            if (t0 > t0a && t0 > t1a) {
                t0 = t0a ;
                return true;
            }
            
            return true;

        }else { // si on n'intersecte pas la sphère B, on ne traite que la sphère A donc retour à une sphère normale**/
            t0 = t0a ;
            return true;
        }
    }
};

bool ray_intersect_bbox(const Vec3f &orig, const Vec3f &dir, const Vec3f &bmin, const Vec3f &bmax){
    float txmin, txmax;
    txmin = (bmin.x - orig.x)   / dir.x;
    txmax = (bmax.x - orig.x) / dir.x; 
    
    if (txmin > txmax) std::swap(txmin, txmax);
    
    float tymin = (bmin.y - orig.y)    / dir.y ;
    float tymax = (bmax.y - orig.y ) / dir.y;
    
   if (tymin > tymax) std::swap(tymin, tymax);
   
   if ( (txmin > tymax) || (tymin > txmax) ) return false;
   
   if ( tymin > txmin   )   txmin = tymin  ;
   if ( tymax < txmax )   txmax = tymax;
    
    float tzmin, tzmax;
    tzmin = (bmin.z - orig.z)   / dir.z;
    tzmax = (bmax.z - orig.z) / dir.z;
    
    if (tzmin > tzmax) std::swap(tzmin, tzmax);
    
    if ( (txmin > tzmax) || (tzmin > txmax) ) return false;
    
    return true;
}

Vec3f reflect(const Vec3f &I, const Vec3f &N) {
    return I - N*2.f*(I*N);
}

Vec3f refract(const Vec3f &I, const Vec3f &N, const float eta_t, const float eta_i=1.f) { // Snell's law
    float cosi = - std::max(-1.f, std::min(1.f, I*N));
    if (cosi<0) return refract(I, -N, eta_i, eta_t); // if the ray comes from the inside the object, swap the air and the media
    float eta = eta_i / eta_t;
    float k = 1 - eta*eta*(1 - cosi*cosi);
    return k<0 ? Vec3f(1,0,0) : I*eta + N*(eta*cosi - sqrtf(k)); // k<0 = total reflection, no ray to refract. I refract it anyways, this has no physical meaning
}

// --------------------------------------- Init Ajouts ---------------------------------------
Model duck("../duck.obj");
Vec3f bmax; // point max de la bbox
Vec3f bmin; // point min de la bbox

float distance = 2.5;
Vec3f centre_ds = Vec3f(1.5, -0.5, -18);

Sphere grandeSphere = Sphere( centre_ds, 3.5, Material(1.0, Vec4f(0.9,  0.1, 0.0, 0.0), Vec3f(0.1, 0.1, 0.1),   10.));
Sphere petiteSphere = Sphere( Vec3f( centre_ds.x + distance, centre_ds.y + distance, centre_ds.z + distance + distance /2), 2.0, Material(1.0, Vec4f(0.6,  0.3, 0.1, 0.0), Vec3f(0.4, 0.4, 0.3),   50.) );

DeathSphere ds = DeathSphere( grandeSphere, petiteSphere);

bool scene_intersect(const Vec3f &orig, const Vec3f &dir, const std::vector<Sphere> &spheres, Vec3f &hit, Vec3f &N, Material &material) {
    float spheres_dist = std::numeric_limits<float>::max();
    float checkerboard_dist = std::numeric_limits<float>::max();
    float duck_dist = std::numeric_limits<float>::max();
    float ds_dist = std::numeric_limits<float>::max();
    
    for (size_t i=0; i < spheres.size(); i++) {
        float dist_i;
        if (spheres[i].ray_intersect(orig, dir, dist_i) && dist_i < spheres_dist) {
            spheres_dist = dist_i;
            hit = orig + dir*dist_i;
            N = (hit - spheres[i].center).normalize();
            material = spheres[i].material;
        }
    }

    // Death Star
    float dist_k;
    if (ds.ray_intersect(orig, dir, dist_k) && dist_k < spheres_dist) {
           ds_dist = dist_k;
           hit = orig + dir*ds_dist;
           N = (hit - ds.sphereA.center).normalize();
           material = ds.sphereA.material;
    }
    
    
    // Canard
        // On affiche chaque face triangulée du petit canard
        for (int i = 0; i < duck.nfaces() ; i++){
            float dist_j;
            
            // Points 3D a, b,c de la face n°i
            Vec3f ab, ac, a, b, c;
            a = duck.point(duck.vert(i, 0));
            b = duck.point(duck.vert(i, 1));
            c = duck.point(duck.vert(i, 2));
            ab = a - b;
            ac = a - c;
            
            if (ray_intersect_bbox(orig, dir, bmin, bmax)){
                if (duck.ray_triangle_intersect(i, orig, dir, dist_j) && dist_j < duck_dist && dist_j < spheres_dist && dist_j < ds_dist ){
                    duck_dist = dist_j;
                    hit = orig + dir*dist_j; 
                    N = cross(ab, ac).normalize(); // le pdt vectoriel donne la normale à ab et ac
                    material = Material(1.5, Vec4f(0.0,  0.5, 0.1, 0.8), Vec3f(0.6, 0.7, 0.8),  125.);
              }
            }         
        }

    if (fabs(dir.y)>1e-3)  {
        float d = -(orig.y+4)/dir.y; // the checkerboard plane has equation y = -4
        Vec3f pt = orig + dir*d;
        if (d>0 && fabs(pt.x)<10 && pt.z<-10 && pt.z>-30 && d<spheres_dist && d < duck_dist && d < ds_dist) {
            checkerboard_dist = d;
            hit = pt;
            N = Vec3f(0,1,0);
            material.diffuse_color = (int(.5*hit.x+1000) + int(.5*hit.z)) & 1 ? Vec3f(.3, .3, .3) : Vec3f(.3, .2, .1);
        }
    }
    
    float min = std::min(spheres_dist, checkerboard_dist);
    min = std::min(min, duck_dist);
    min = std::min(min, ds_dist);
    return min <1000;
}

Vec3f cast_ray(const Vec3f &orig, const Vec3f &dir, const std::vector<Sphere> &spheres, const std::vector<Light> &lights, size_t depth=0) {
    Vec3f point, N;
    Material material;

    if (depth>4 || !scene_intersect(orig, dir, spheres, point, N, material)) {
        int a = std::max(0, std::min(envmap_width -1, static_cast<int>((atan2(dir.z, dir.x)/(2*M_PI) + .5)*envmap_width)));
        int b = std::max(0, std::min(envmap_height-1, static_cast<int>(acos(dir.y)/M_PI*envmap_height)));
        return envmap[a+b*envmap_width]; // background color
    }

    Vec3f reflect_dir = reflect(dir, N).normalize();
    Vec3f refract_dir = refract(dir, N, material.refractive_index).normalize();
    Vec3f reflect_orig = reflect_dir*N < 0 ? point - N*1e-3 : point + N*1e-3; // offset the original point to avoid occlusion by the object itself
    Vec3f refract_orig = refract_dir*N < 0 ? point - N*1e-3 : point + N*1e-3;
    Vec3f reflect_color = cast_ray(reflect_orig, reflect_dir, spheres, lights, depth + 1);
    Vec3f refract_color = cast_ray(refract_orig, refract_dir, spheres, lights, depth + 1);

    float diffuse_light_intensity = 0, specular_light_intensity = 0;
    for (size_t i=0; i<lights.size(); i++) {
        Vec3f light_dir      = (lights[i].position - point).normalize();
        float light_distance = (lights[i].position - point).norm();

        Vec3f shadow_orig = light_dir*N < 0 ? point - N*1e-3 : point + N*1e-3; // checking if the point lies in the shadow of the lights[i]
        Vec3f shadow_pt, shadow_N;
        Material tmpmaterial;
        if (scene_intersect(shadow_orig, light_dir, spheres, shadow_pt, shadow_N, tmpmaterial) && (shadow_pt-shadow_orig).norm() < light_distance)
            continue;

        diffuse_light_intensity  += lights[i].intensity * std::max(0.f, light_dir*N);
        specular_light_intensity += powf(std::max(0.f, -reflect(-light_dir, N)*dir), material.specular_exponent)*lights[i].intensity;
    }
    return material.diffuse_color * diffuse_light_intensity * material.albedo[0] + Vec3f(1., 1., 1.)*specular_light_intensity * material.albedo[1] + reflect_color*material.albedo[2] + refract_color*material.albedo[3];
}

void render(const std::vector<Sphere> &spheres, const std::vector<Light> &lights) {
    const int   width    = 1024;
    const int   height   = 768;
    const float fov      = M_PI/3.;
    
    std::vector<unsigned char> pixmap(width*height*3); 
    std::vector<Vec3f> framebuffer(width*height);

    #pragma omp parallel for
    for (size_t j = 0; j<height; j++) { // actual rendering loop
        for (size_t i = 0; i<width; i++) { 
        
            float dir_x =  (i + 0.5) -  width/2. ;
            float dir_y = -(j + 0.5) + height/2.;    // this flips the image at the same time
            float dir_z = -height/(2.*tan(fov/2.));

            framebuffer[i + j*width] = cast_ray(Vec3f(0,0,3), Vec3f(dir_x, dir_y, dir_z).normalize(), spheres, lights);
                                                        
        }
    }

    for (size_t i = 0; i < height*width; ++i) {
        Vec3f &c = framebuffer[i];
        float max = std::max(c[0], std::max(c[1], c[2]));
        if (max>1) c = c*(1./max);
        for (size_t j = 0; j<3; j++) {
            pixmap[i*3+j] = (unsigned char)(255 * std::max(0.f, std::min(1.f, framebuffer[i][j])));
        }
    }
    
    stbi_write_jpg("out.jpg", width, height, 3, pixmap.data(), 100);

}

int main() {
    int n = -1;
    unsigned char *pixmap = stbi_load("../envmap.jpg", &envmap_width, &envmap_height, &n, 0);
    if (!pixmap || 3!=n) {
        std::cerr << "Error: can not load the environment map" << std::endl;
        return -1;
    }
    envmap = std::vector<Vec3f>(envmap_width*envmap_height);
    for (int j = envmap_height-1; j>=0 ; j--) {
        for (int i = 0; i<envmap_width; i++) {
            envmap[i+j*envmap_width] = Vec3f(pixmap[(i+j*envmap_width)*3+0], pixmap[(i+j*envmap_width)*3+1], pixmap[(i+j*envmap_width)*3+2])*(1/255.);
        }
    }
    stbi_image_free(pixmap);

    Material      ivory(1.0, Vec4f(0.6,  0.3, 0.1, 0.0), Vec3f(0.4, 0.4, 0.3),   50.);
    Material      glass(1.5, Vec4f(0.0,  0.5, 0.1, 0.8), Vec3f(0.6, 0.7, 0.8),  125.);
    Material      red_rubber(1.0, Vec4f(0.9,  0.1, 0.0, 0.0), Vec3f(0.3, 0.1, 0.1),   10.);
    Material     mirror(1.0, Vec4f(0.0, 10.0, 0.8, 0.0), Vec3f(1.0, 1.0, 1.0), 1425.);

    std::vector<Sphere> spheres;
    spheres.push_back(Sphere(Vec3f(-3,    0,   -16), 2,      ivory));
    spheres.push_back(Sphere(Vec3f(-1.0, -1.5, -12), 2,      glass));
    //spheres.push_back(Sphere(Vec3f( 1.5, -0.5, -18), 3, red_rubber));
    spheres.push_back(Sphere(Vec3f( 7,    5,   -18), 4,     mirror));
    

    std::vector<Light>  lights;
    lights.push_back(Light(Vec3f(-20, 20,  20), 1.5));
    lights.push_back(Light(Vec3f( 30, 50, -25), 1.8));
    lights.push_back(Light(Vec3f( 30, 20,  30), 1.7));
    
    duck.get_bbox(bmin, bmax);

    render(spheres, lights);

    return 0;
}

