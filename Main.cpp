#include <stdio.h>
#include <windows.h>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <iostream>
#include "glsl.h"
#include <GLFW/glfw3.h>
#include "Basic.h"
#include "glut.h"
#include "shlwapi.h"
#include "ILMBase.h"
#include "SphereMesh.h"
#include "Depth.h"
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include <cstdlib>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

using namespace DepthNamespace;
using namespace std;
GLFWwindow* g_window = NULL;
int g_window_w = 1900;
int g_window_h = 1060;

extern Vec2f g_zenith_range;

//SaveCubemap setting
vector<Vec4f> g_cubemap_FOVs;
vector<Vec4f> g_cubemap_ranges;

//shader stuff
cwc::glShaderManager* g_shaderManager = NULL;
cwc::glShader* g_shader_perspective = NULL;  //for perspective rendering
cwc::glShader* g_shader_perspective_texcoord = NULL;  //for perspective rendering, that directly use a pixel's texcoords

//spherical modeing stuff
//(one for each L/R)
struct Spherical
{
	SphereNamespace::LiteMesh LM;
	GLuint cid;  //cubemap texture id. 0:nope	
};
Spherical g_L;

void AllFilesInFolder(const char* filename, vector<string>& all_filenames)
{
	//get folder name first
	char* folder = new char[strlen(filename) + 1];
	memset(folder, NULL, sizeof(char) * strlen(filename));
	strcpy(folder, filename);
	PathRemoveFileSpecA(folder);
	string folder_str = folder;  //append "\\*" to the end
	folder_str += "\\*";

	HANDLE hFind = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATAA ffd;
	hFind = FindFirstFileA(folder_str.c_str(), &ffd);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				//directory. skip
			}
			else
			{
				string fn = folder;
				fn += "\\";
				fn += ffd.cFileName;  //only the file name
				all_filenames.push_back(fn);
			}
		} while (FindNextFileA(hFind, &ffd) != 0);
	}
	FindClose(hFind);

	delete folder;
}

//render the current scene to a cubemap (6 texture buffers)
//setting (for save_image mode): {azi_left,azi_right,zen_up,zen_down} for the first "horizontal" cubemap faces 
bool CreateCubeMap(Spherical& S, bool save_image, vector<Vec4f>* setting = NULL)
{
	//cubemap_size: the width and height of each face (square)
	int cubemap_size = 512;

	//setup the framebuffer stuff:
	unsigned int FramebufferName = 0;
	glGenFramebuffersEXT(1, &FramebufferName);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, FramebufferName);

	//recreate the cubemap textures and data:	
	if (glIsTexture(S.cid))
		glDeleteTextures(1, &S.cid);
	glGenTextures(1, &S.cid);
	glBindTexture(GL_TEXTURE_CUBE_MAP, S.cid);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	for (int face = 0; face < 6; face++)
	{
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB, cubemap_size, cubemap_size, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
	}

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, FramebufferName);

	for (int face = 0; face < 6; face++)
	{
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
			S.cid, 0/*midmap level, must be 0*/);

		GLenum status = glCheckFramebufferStatusEXT(GL_DRAW_FRAMEBUFFER_EXT);
		if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
		{
			printf("[CreateCubemap] Framebuffer status error: %08x\n", status);
			return false;
		}

		//setup the look at:
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		Vec3f dir(0), up(0);
		if (face == 0)  //+x
		{
			dir = Vec3f(1, 0, 0);
			up = Vec3f(0, -1, 0);
		}
		else if (face == 1)  //-x
		{
			dir = Vec3f(-1, 0, 0);
			up = Vec3f(0, -1, 0);
		}
		else if (face == 2)  //+y
		{
			dir = Vec3f(0, 1, 0);
			up = Vec3f(0, 0, 1);
		}
		else if (face == 3)  //-y
		{
			dir = Vec3f(0, -1, 0);
			up = Vec3f(0, 0, -1);
		}
		else if (face == 4)  //+z ("top")
		{
			dir = Vec3f(0, 0, 1);
			up = Vec3f(0, -1, 0);
		}
		else if (face == 5)  //-z ("bottom")
		{
			dir = Vec3f(0, 0, -1);
			up = Vec3f(0, -1, 0);
		}

		float fovx = 90;   //degree
		float aspect = 1;  //w/h aspect ratio

		if (setting)  //customized settings for the 4 horizontal faces?
		{
			if (face < (*setting).size())
			{
				float azi_center = ((*setting)[face][1] + (*setting)[face][0]) / 2;
				fovx = R2D((*setting)[face][1] - (*setting)[face][0]);
				float fovy = R2D((*setting)[face][3] - (*setting)[face][2]);
				aspect = tan(D2R(fovx) / 2) / tan(D2R(fovy) / 2);

				//dir is (cos(azi),sin(azi),0)
				dir = Vec3f(cos(azi_center), sin(azi_center), 0);

				//cout << "[CreateCubeMap] face#" << face << " azi_center:" << R2D(azi_center) << " fovx:" << fovx << " fovy:" << fovy << 
				//	" aspect:" << aspect << " dir:" << dir << endl;

				//note: up is always (0,0,1)
				up = Vec3f(0, 0, 1);
			}
		}

		gluLookAt(0, 0, 0, dir.x, dir.y, dir.z, up.x, up.y, up.z);

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		gluPerspective(fovx, aspect, 0.1, 1000);

		Vec4i viewport(0, 0, cubemap_size, cubemap_size);
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);


		//DrawSphereMesh(S, 1);



		//save current frame buffer to image file?
		if (save_image)
		{
			unsigned char* data = new unsigned char[viewport[2] * viewport[3] * 3];
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, data);

			char filename[260] = { NULL };
			sprintf(filename, "cubemap%d.jpg", face);
			stbi_flip_vertically_on_write(true);
			stbi_write_jpg(filename, viewport[2], viewport[3], 3, data, viewport[2] * 3/*stride*/);
			delete data;
		}
	}

	glDeleteFramebuffersEXT(1, &FramebufferName);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

	return true;
}


void DrawLiteMesh(Spherical& S, float alpha, glm::mat4 projMat, glm::mat4 viewMat, glm::mat4 modelMat)
{
	//glBindTexture(GL_TEXTURE_2D, S.LM.m_texture.id);
	//glBindTexture(GL_TEXTURE_1D, S.sdepth_texid);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	//g_shader_perspective_texcoord->begin();
	//g_shader_perspective_texcoord->setUniform1i((char*)"draw_texture", (int)1);
	//g_shader_perspective_texcoord->setUniform1f((char*)"alpha", alpha);

	//shader language
	const char* vertexShaderSource =
		"#version 460 core\n"
		"layout (location = 0) in vec3 aPos;\n"
		"layout (location = 1) in vec2 aUV;\n"
		"uniform mat4 projMat;\n"
		"uniform mat4 viewMat;\n"
		"uniform mat4 modelMat;\n"
		"out vec2 uv;\n"
		"void main()\n"
		"{\n"
		"	//gl_Position = vec4(aPos.x, aPos.y, aPos.z, 1.0f);\n"
		"	gl_Position = projMat * viewMat * modelMat * vec4(aPos, 1.0);\n"
		"	uv = aUV;\n"
		"}\0";
	const char* fragmentShaderSource =
		"#version 460 core\n"
		"in vec2 uv;\n"
		"out vec4 FragColor;\n"
		"uniform sampler2D sampler;\n"
		"void main()\n"
		"{\n"
		"	//FragColor = vec4(0.2f, 0.5f, 1.0f, 1.0f);\n"
		"	FragColor = texture(sampler, uv);\n"
		"}\n\0";

	//create shaders
	GLuint vertex, fragment;
	vertex = glCreateShader(GL_VERTEX_SHADER);
	fragment = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(vertex, 1, &vertexShaderSource, NULL);
	glShaderSource(fragment, 1, &fragmentShaderSource, NULL);

	//complie shaders and check
	int success = 0;
	char infoLog[1024];

	glCompileShader(vertex);
	glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
	if (!success) {
		glGetShaderInfoLog(vertex, 1024, NULL, infoLog);
		cout << "error: vertex shader compile error" << "\n" << infoLog << endl;
	}

	glCompileShader(fragment);
	glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
	if (!success) {
		glGetShaderInfoLog(fragment, 1024, NULL, infoLog);
		cout << "error: fragment shader compile error" << "\n" << infoLog << endl;
	}

	//create  program
	GLuint program = 0;
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);

	//link program and check
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success) {
		glGetProgramInfoLog(program, 1024, NULL, infoLog);
		cout << "error: shader link error" << "\n" << infoLog << endl;
	}
	glDeleteShader(vertex);
	glDeleteShader(fragment);

	glUseProgram(program);

	//set the value of the uniform matrix
	GLuint modelMatIdx = glGetUniformLocation(program, "modelMat");
	GLuint viewMatIdx = glGetUniformLocation(program, "viewMat");
	GLuint projMatIdx = glGetUniformLocation(program, "projMat");

	glUniformMatrix4fv(projMatIdx, 1, GL_FALSE, glm::value_ptr(projMat));
	glUniformMatrix4fv(modelMatIdx, 1, GL_FALSE, glm::value_ptr(modelMat));
	glUniformMatrix4fv(viewMatIdx, 1, GL_FALSE, glm::value_ptr(viewMat));

	//set the value of the sampler
	GLuint samplerIdx = glGetUniformLocation(program, "sampler");

	glUniform1i(samplerIdx, 0);

	S.LM.DrawArray();

	//g_shader_perspective_texcoord->end();

	glDisable(GL_BLEND);
}

//save horizontal cubemap faces (i.e., up = (0,0,1)) to files
//setting: {azi_left,azi_right,zen_up,zen_down} for the "horizontal" cubemap faces 
bool SaveCubeMap(Spherical& S, vector<Vec4f>& settings, string filename_head)
{
	for (int face = 0; face < settings.size(); face++)
	{
		//setup dir 
		Vec3f dir;
		float azi_center = (settings[face][1] + settings[face][0]) / 2;
		float zen_center = (settings[face][3] + settings[face][2]) / 2;
		float fovx = R2D(settings[face][1] - settings[face][0]);  //degree
		float fovy = R2D(settings[face][3] - settings[face][2]);
		float aspect = tan(D2R(fovx) / 2) / tan(D2R(fovy) / 2);  //w/h aspect ratio

		//dir is by spherical coord.
		dir = Vec3f(cos(azi_center) * sin(zen_center), sin(azi_center) * sin(zen_center), cos(zen_center));

		//note: up is always (0,0,1)
		Vec3f up = Vec3f(0, 0, 1);

		//setup the look at:
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		//use current PCamera's center:
		gluLookAt(0, 0, 0, dir.x, dir.y, dir.z, up.x, up.y, up.z);

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		gluPerspective(fovy, aspect, 0.1, 1000);

		glm::mat4 projMat = glm::perspective(glm::radians(fovy), aspect, 0.1f, 1000.0f);
		glm::mat4 viewMat = glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(dir.x, dir.y, dir.z), glm::vec3(up.x, up.y, up.z));
		glm::mat4 modelMat = glm::mat4(1.0f);
		//cout << "aspect " << aspect << ", fovy " << fovy << ", up " << up << ", dir " << dir << endl;

		int viewport_width = 1024;
		int viewport_height = round((float)viewport_width / aspect);

		//note: viewport cannot be larger than the window size!
		if (aspect >= 1)
		{
			//width viewport
			if (viewport_width > g_window_w - 50)
			{
				viewport_width = g_window_w - 50;
				viewport_height = round((float)viewport_width / aspect);
			}
		}
		else
		{
			//tall viewport
			if (viewport_height > g_window_h - 50) {
				viewport_height = g_window_h - 50;
				viewport_width = round((float)viewport_height * aspect);
			}
		}

		Vec4i viewport(0, 0, viewport_width, viewport_height);
		//cout << "viewport " << viewport << endl;
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

		cout << "[SaveCubeMap] face#" << face << " azi_center:" << R2D(azi_center) << " fovx:" << fovx << " fovy:" << fovy <<
			" aspect:" << aspect << " dir:" << dir << " w:" << viewport_width << " h:" << viewport_height << endl;

		glClearColor(1, 1, 1, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


		DrawLiteMesh(S, 1, projMat, viewMat, modelMat);

		//save current frame buffer to image file
		{
			unsigned char* data = new unsigned char[viewport[2] * viewport[3] * 3];
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, data);

			//prepare filename:
			char filename[260] = { NULL };
			sprintf(filename, "%s.%d_%d_%d_%d.jpg", filename_head.c_str(),
				(int)round(R2D(settings[face][0])), (int)round(R2D(settings[face][1])),
				(int)round(R2D(settings[face][2])), (int)round(R2D(settings[face][3])));

			cout << filename << endl;

			stbi_flip_vertically_on_write(true);
			stbi_write_jpg(filename, viewport[2], viewport[3], 3, data, viewport[2] * 3/*stride*/);
			delete data;
		}
	}

	return true;
}


bool CreateDepthPanoramas(int argc, char* argv[], bool metric_cal_only, bool skip_createPano, bool skip_depth_pred, bool skip_merge)
{
	if (argc < 6)
	{
		cout << "[CreateDepthPanormas] error: need argc>=6" << endl;
		return false;
	}

	//assume baseline depth panoramas have been generated by other methods (e.g., BiFuse) previously

	//*assume this is run in the same folder as MiDas, and called from MiDas's Anaconda environement

	//we need folder names for 1) RGB panoramas, 2) gt depth panoramas, 3) baseline depth panoramas, 4) result depth panoramas  
	//as argv[1], argv[2], argv[3]

	string RGB_folder(argv[2]);
	string gt_folder(argv[3]);
	string baseline_folder(argv[4]);
	string result_folder(argv[5]);

	string test_img_folder;
	string output_txt;
	bool save_metric = true;
	if (save_metric) output_txt = ".\\record\\matterport3d\\baseline_unifuse\\depthAnything_metric_raw_margin10\\metrics.txt";


	//set to another folder
	// matterport3d
	baseline_folder = ".\\\\datas\\\\matterport3d\\\\egformer\\\\result\\\\";
	RGB_folder = ".\\\\datas\\\\matterport3d\\\\rgb\\\\";
	gt_folder = ".\\\\datas\\\\matterport3d\\\\gt_grayscale\\\\";
	result_folder = ".\\\\record\\\\matterport3d\\\\baseline_unifuse\\\\depthAnything_metric_raw_margin10\\\\result\\\\";
	test_img_folder = "test_images_matterport3d/test_images_depthAnyting_metric_raw_margin10/";

	// replica360 2k
	/*baseline_folder = ".\\\\datas\\\\replica360_2k\\\\hohonet\\\\result\\\\";
	RGB_folder = ".\\\\datas\\\\replica360_2k\\\\replica3602k_rgb\\\\";
	gt_folder = ".\\\\datas\\\\replica360_2k\\\\replica3602k_gt\\\\";
	result_folder = ".\\\\record\\\\replica360_2k\\\\baseline_hohonet\\\\leres\\\\result\\\\";*/

	cout << "[CreateDepthPanormas] argc=" << argc << " RGB_folder:" << RGB_folder << " gt_folder:" << gt_folder <<
		" baseline_folder:" << baseline_folder << " result_folder:" << result_folder << endl;

	cout << "metric_cal_only: " << metric_cal_only << " skip create pano: " << skip_createPano << " skip_depth_pred: " <<
		skip_depth_pred << " skip_merge: " << skip_merge << endl;

	//return true;

	////let's collect all file names in the RGB folder!
	vector<string> RGB_filenames;
	AllFilesInFolder(RGB_folder.c_str(), RGB_filenames);
	//cout << "filename" << endl;
	//for (auto filename : RGB_filenames) cout << filename << endl;
	//cout << "filename" << endl;

	if (metric_cal_only) {
		cout << "metric cal only" << endl;
		bool replica = false;

		// matterport3d
		string gt_folder = ".\\datas\\matterport3d\\gt_grayscale\\";
		//string pred_folder = ".\\record\\matterport3d\\baseline_unifuse\\zoe_raw\\result\\";
		string pred_folder = ".\\datas\\matterport3d\\panoformer\\result\\"; // baseline 
		//output_txt = ".\\record\\matterport3d\\baseline_unifuse\\zoe_raw\\metrics.txt";
		string output_txt = ".\\datas\\matterport3d\\panoformer\\result\\metrics.txt";

		// replica360 2k
		//replica = true;
		//string gt_folder = ".\\datas\\replica360_2k\\replica3602k_gt\\";
		//string pred_folder = ".\\record\\replica360_2k\\baseline_hohonet\\leres\\\\result\\"; // hohonet
		//string output_txt = ".\\record\\replica360_2k\\baseline_hohonet\\leres\\\\result\\\\metrics.txt"; //hohonet

		vector<string> gts;
		AllFilesInFolder(gt_folder.c_str(), gts);
		//cout << gts[0] << endl;


		float rmse_given_avg = 0, rmse_result_avg = 0, mae_given_avg = 0, mae_result_avg = 0,
			mre_given_avg = 0, mre_result_avg = 0, rmselog_given_avg = 0, rmselog_result_avg = 0;
		float delta1_given_avg = 0, delta1_result_avg = 0, delta2_given_avg = 0, delta2_result_avg = 0, delta3_given_avg = 0, delta3_result_avg = 0;

		float img_num = 0, load_error_num = 0;

		for (auto gt : gts) {
			string pred;
			string rawname;
			char* filename = PathFindFileNameA(gt.c_str());

			// matterport3d
			rawname = string(filename).substr(0, 4);
			pred = pred_folder + rawname + ".png"; //for merged pred pano and egformer panoformer
			//pred = pred_folder + rawname + "_rgb.depth.png"; // hohonet
			//pred = pred_folder + rawname + "_depth_pred.jpg"; //for pred of baseline of unifuse / depthAnything
			//pred = pred_folder + rawname + "_depth_pred.png"; //for pred of baseline of zoe / depthAnything metric

			// replica360 2k
			//std::string suffix = "_depth_pano.pfm";
			//size_t suffixPos = string(filename).find(suffix);
			//rawname = string(filename).substr(0, suffixPos);
			//pred = pred_folder + rawname + "_rgb_pano.png"; // leres (hohonet)
			//pred = pred_folder + rawname + "_rgb_pano.depth.png"; // hohonet


			int align_way = 1;
			//align_way = 0; // no align
			//load the ground-truth emap:
			EquirectangularMap emap_gt;
			EquirectangularMap emap_pred;

			vector<DepthNamespace::Metrics> metrics_aligned_all;
			DepthNamespace::Metrics metrics_aligned;

			bool cap_depth = true, DispDepthCompare = false;

			//the pred is disp
			if (DispDepthCompare) {
				string shifted_filename_str = pred_folder + "shifted\\\\" + rawname + "_depth_pred.png";
				const char* shifted_filename = shifted_filename_str.c_str();
				ErrorCompare(gt, pred, DispDepthCompare, metrics_aligned.mse_result, metrics_aligned.mae_result, metrics_aligned.mre_result,
					metrics_aligned.mselog_result, metrics_aligned.delta1_result, metrics_aligned.delta2_result, metrics_aligned.delta3_result, align_way, cap_depth, shifted_filename);

				metrics_aligned_all.push_back(metrics_aligned);


				rmse_given_avg += sqrt(metrics_aligned.mse_given);
				rmse_result_avg += sqrt(metrics_aligned.mse_result);
				mae_given_avg += metrics_aligned.mae_given;
				mae_result_avg += metrics_aligned.mae_result;
				mre_given_avg += metrics_aligned.mre_given;
				mre_result_avg += metrics_aligned.mre_result;
				rmselog_given_avg += sqrt(metrics_aligned.mselog_given);
				rmselog_result_avg += sqrt(metrics_aligned.mselog_result);
				delta1_given_avg += metrics_aligned.delta1_given;
				delta1_result_avg += metrics_aligned.delta1_result;
				delta2_given_avg += metrics_aligned.delta2_given;
				delta2_result_avg += metrics_aligned.delta2_result;
				delta3_given_avg += metrics_aligned.delta3_given;
				delta3_result_avg += metrics_aligned.delta3_result;

				img_num++;
			}
			else {
				if (emap_gt.Load((gt)) && emap_pred.Load((pred)))
				{
					//emap_pred.DispDepthConversion(); // zoe, convert disp to depth
					ErrorEmap(emap_gt, emap_pred, metrics_aligned.mse_result, metrics_aligned.mae_result, metrics_aligned.mre_result,
						metrics_aligned.mselog_result, metrics_aligned.delta1_result, metrics_aligned.delta2_result, metrics_aligned.delta3_result, align_way, cap_depth);

					metrics_aligned_all.push_back(metrics_aligned);


					rmse_given_avg += sqrt(metrics_aligned.mse_given);
					rmse_result_avg += sqrt(metrics_aligned.mse_result);
					mae_given_avg += metrics_aligned.mae_given;
					mae_result_avg += metrics_aligned.mae_result;
					mre_given_avg += metrics_aligned.mre_given;
					mre_result_avg += metrics_aligned.mre_result;
					rmselog_given_avg += sqrt(metrics_aligned.mselog_given);
					rmselog_result_avg += sqrt(metrics_aligned.mselog_result);
					delta1_given_avg += metrics_aligned.delta1_given;
					delta1_result_avg += metrics_aligned.delta1_result;
					delta2_given_avg += metrics_aligned.delta2_given;
					delta2_result_avg += metrics_aligned.delta2_result;
					delta3_given_avg += metrics_aligned.delta3_given;
					delta3_result_avg += metrics_aligned.delta3_result;

					img_num++;

				}
				else {
					cout << "load error" << endl;
					load_error_num++; // replica360 2k / there are some imgs cannt load
					if (!replica) {
						return true;
					}
				}
			}
			cout << gt << "   " << pred << endl;
		}

		rmse_given_avg /= img_num;
		rmse_result_avg /= img_num;
		mae_given_avg /= img_num;
		mae_result_avg /= img_num;
		mre_given_avg /= img_num;
		mre_result_avg /= img_num;
		rmselog_given_avg /= img_num;
		rmselog_result_avg /= img_num;
		delta1_given_avg /= img_num;
		delta1_result_avg /= img_num;
		delta2_given_avg /= img_num;
		delta2_result_avg /= img_num;
		delta3_given_avg /= img_num;
		delta3_result_avg /= img_num;

		cout << "RMSE_given:" << rmse_given_avg << " RMSE_result:" << rmse_result_avg << " (" <<
			(rmse_result_avg - rmse_given_avg) / rmse_given_avg << ")" <<
			" MAE_given:" << mae_given_avg << " MAE_result_avg:" << mae_result_avg << " (" <<
			(mae_result_avg - mae_given_avg) / mae_given_avg << ")" <<
			" MRE_given:" << mre_given_avg << " MRE_result_avg:" << mre_result_avg << " (" <<
			(mre_result_avg - mre_given_avg) / mre_given_avg << ")" <<
			" RMSElog_given:" << rmselog_given_avg << " RMSElog_result:" << rmselog_result_avg << " (" <<
			(rmselog_result_avg - rmselog_given_avg) / rmselog_given_avg << ")" <<
			" delta1_given:" << delta1_given_avg << " delta1_result:" << delta1_result_avg << " (" <<
			(delta1_result_avg - delta1_given_avg) << ")" <<
			" delta2_given:" << delta2_given_avg << " delta2_result:" << delta2_result_avg << " (" <<
			(delta2_result_avg - delta2_given_avg) << ")" <<
			" delta3_given:" << delta3_given_avg << " delta3_result:" << delta3_result_avg << " (" <<
			(delta3_result_avg - delta3_given_avg) << ")" << endl;

		ofstream outFile(output_txt);

		if (!outFile) {
			std::cerr << "fail to open file" << std::endl;
			return 1;
		}
		outFile << " RMSE: " << rmse_result_avg << endl;
		outFile << " MAE: " << mae_result_avg << endl;
		outFile << " MRE: " << mre_result_avg << endl;
		outFile << " RMSElog: " << rmselog_result_avg << endl;
		outFile << " delta1: " << delta1_result_avg << endl;
		outFile << " delta2: " << delta2_result_avg << endl;
		outFile << " delta3: " << delta3_result_avg << endl;

		outFile.close();

		cout << "error counts: " << load_error_num << endl;

		return true;
	}

	//only do some?
	if (false)
	{
		vector<string> RGB_filenames_new;

		for (int i = 0; i < RGB_filenames.size(); i++)
		{
			if (RGB_filenames[i].find("030de5aee3f942edb95676ad2724cdac") != string::npos ||
				RGB_filenames[i].find("001ad2ad14234e06b2d996d71bb96fc4") != string::npos ||
				RGB_filenames[i].find("00af93c06521455ea528309996881b8d") != string::npos)
				RGB_filenames_new.push_back(RGB_filenames[i]);
		}

		RGB_filenames = RGB_filenames_new;
	}

	//only do a couple?
	if (false)
	{
		vector<string> RGB_filenames_new;

		for (int i = 0; i < RGB_filenames.size(); i++)
		{
			RGB_filenames_new.push_back(RGB_filenames[i]);

			if (RGB_filenames_new.size() >= 100)
				break;
		}

		RGB_filenames = RGB_filenames_new;
	}

	//skip certain cases? (missing in Midas)
	if (false)
	{
		vector<string> RGB_filenames_new;

		for (int i = 0; i < RGB_filenames.size(); i++)
		{
			if (RGB_filenames[i].find("7dcb06593614423493e62ba37406aab1") != string::npos ||
				RGB_filenames[i].find("bd938f308f6243ba922fe3b69c262e58") != string::npos ||
				RGB_filenames[i].find("d278fb01ab624a88b3c8db166c91c06a") != string::npos)
			{
				cout << "Skip: " << RGB_filenames[i] << endl;
				continue;
			}
			else
				RGB_filenames_new.push_back(RGB_filenames[i]);
		}

		RGB_filenames = RGB_filenames_new;
	}

	cout << "[CreateDepthPanormas] #RGB_filenames:" << RGB_filenames.size() << endl;

	//for every RGB panorama, create a set of sub-perspective images
	DWORD time = timeGetTime();
	bool leres = false;
	bool zoedepth = false;
	bool depthAnything = false;
	if (!skip_createPano) {
		if (true)
		{
			// Setup window
			if (!glfwInit())
				return false;

			//windowed full-screen mode
			const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
			g_window_w = mode->width - 15;  //note: leave a bit padding
			g_window_h = mode->height - 15;

			//opengl version setting
			//glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
			//glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
			//glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);

			g_window = glfwCreateWindow(g_window_w, g_window_h, "Pano", NULL, NULL);
			glfwMakeContextCurrent(g_window);

			if (g_shaderManager == NULL)
			{
				//initialize shader:
				g_shaderManager = new cwc::glShaderManager();

				g_shader_perspective_texcoord = g_shaderManager->loadfromFile((char*)"shaders/vs.txt", (char*)"shaders/fs_perspective_texcoord.txt");
				if (g_shader_perspective_texcoord == 0)
					std::cout << "Error: cannot load shader_perspective_texcoord" << std::endl;
			}

			//work on every input RGB panorama, create the perspective images:
			for (int i = 0; i < RGB_filenames.size(); i++)
			{
				//use the left sphere mesh's LM for drawing
				g_L.LM.InitSphere(180, 90);

				//take the RGB panorama as the main texture
				g_L.LM.LoadTexture(RGB_filenames[i].c_str());

				//draw the perspective images:
				{
					glClearColor(1, 1, 1, 1.0f);
					glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

					//get the raw filename w/o extension
					char* filename = PathFindFileNameA(RGB_filenames[i].c_str());
					//size_t lastindex = string(filename).find_last_of(".");
					size_t lastindex = string(filename).find_last_of("_"); // unifuse matterport
					string rawname = string(filename).substr(0, lastindex);
					//cout << filename << endl;
					//cout << lastindex << endl;
					//cout << rawname << endl;
					//cout << "here" << endl;

					//SaveCubeMap(g_L, g_cubemap_FOVs, "input/" + rawname);  //midas

					SaveCubeMap(g_L, g_cubemap_FOVs, "test_images_matterport3d/perspective_rgb_fold4_margin_quarter/" + rawname);  //LeReS
					//SaveCubeMap(g_L, g_cubemap_FOVs, "test_images/" + rawname); //test
				}
			}
		}
		cout << "SaveCubemps done! time:" << timeGetTime() - time << endl;
	}
	else {
		cout << "skip_createPano" << endl;
	}
	if (!skip_depth_pred) {
		time = timeGetTime();
		//call MiDas:
		if (false)
		{
			system("python run.py --model_type dpt_large");
		}
		//call LeRes:
		if (false)
		{
			system("python test_depth.py --load_ckpt res101.pth --backbone resnext101");
		}
		if (false) {
			system("C:\\Users\\user\\miniconda3\\envs\\adabin\\python.exe .\\AdelaiDepth\\LeReS\\Minist_Test\\tools\\test_depth.py");
			cout << "LeReS done! time:" << timeGetTime() - time << endl;
			leres = true;
		}
		// call Adabins:
		if (false)
		{
			system("C:\\Users\\user\\miniconda3\\envs\\adabin\\python.exe .\\AdaBins\\adabin.py");
			cout << "AdaBins Done! time:" << timeGetTime() - time << endl;
		}

		// call ZoeDepth
		if (false) {
			//system("C:\\Users\\user\\miniconda3\\envs\\adabin\\python.exe .\\ZoeDepth\\ZoeDepth.py"); //save the colorized pred img
			system("C:\\Users\\user\\miniconda3\\envs\\adabin\\python.exe .\\ZoeDepth\\ZoeDepth_saveRawImg.py"); // save the raw pred img in uint16
			cout << "ZoeDepth Done! time:" << timeGetTime() - time << endl;
			zoedepth = true;
		}
		// call depth-anything vitl
		if (false) {
			system("C:\\Users\\user\\miniconda3\\envs\\adabin\\python.exe .\\Depth-Anything\\run_vitl.py");
			cout << "depth-anything vitl Done! time:" << timeGetTime() - time << endl;
			depthAnything = true;
		}
		// call depth-anything metric
		if (true) {
			system("C:\\Users\\user\\miniconda3\\envs\\adabin\\python.exe .\\Depth-Anything\\metric_depth\\depth_pred.py");
			cout << "depth-anything metric Done! time:" << timeGetTime() - time << endl;
			depthAnything = true;
		}
		// call depth-anything-V2 metric
		if (false) {
			system("C:\\Users\\user\\miniconda3\\envs\\adabin\\python.exe .\\Depth-Anything-V2\\metric_depth\\run.py");
			cout << "depth-anything metric Done! time:" << timeGetTime() - time << endl;
			depthAnything = true;
		}
	}
	else {
		cout << "skip_depth_pred" << endl;
		leres = true; //using leres to predict the depth of perspective
	}

	//cout << "LeReS done! time:" << timeGetTime() - time << endl;
	time = timeGetTime();

	if (!skip_merge) {
		string baselines[] = { "unifuse", "hohonet" };
		string type = "_fold4";
		string results[] = { "depthAnything_metric_raw" + type, "leres" + type, "zoe_raw" + type, "depthAnythingV2_metric_raw" + type };
		for (auto baseline : baselines) {
			for (auto result : results) {
				if (save_metric) output_txt = ".\\record\\matterport3d\\baseline_" + baseline + "\\" + result + "\\metrics.txt";


				//set to another folder
				// matterport3d
				baseline_folder = ".\\\\datas\\\\matterport3d\\\\" + baseline + "\\\\result\\\\";
				result_folder = ".\\\\record\\\\matterport3d\\\\baseline_" + baseline + "\\\\" + result + "\\\\result\\\\";
				test_img_folder = "test_images_matterport3d/test_images_" + result + "/";
				/*cout << output_txt << endl;
				cout << baseline_folder << endl;
				cout << result_folder << endl;
				cout << test_img_folder << endl;*/


				////for each RGB panorama and its depth perspective images, together with the baseline depth panorama
				//run our depth merging code to create the result depth panoramas
				//put them in the result depth panoramas folder

				vector<DepthNamespace::Metrics> metrics_aligned_all;
				vector<int> time_Regs;
				vector<int> time_Laplacians;

				bool replica = false;

				for (int i = 0; i < RGB_filenames.size(); i++)
				{
					//find the corresponding baseline and gt filenames: same raw filename, but in the gt folder (may have the different file types) 
					string fn_equirectangular_baseline;
					string fn_equirectangular_gt;
					string rawname;
					{
						char* filename = PathFindFileNameA(RGB_filenames[i].c_str());
						//size_t lastindex = string(filename).find_last_of("."); //replica
						size_t lastindex = string(filename).find_last_of("_"); // matterport3d
						rawname = string(filename).substr(0, lastindex);


						//baseline filename?
						//fn_equirectangular_baseline = baseline_folder + rawname + ".jpg";  //bifuse
						if (baseline == "unifuse") {
							fn_equirectangular_baseline = baseline_folder + rawname + "_depth_pred.jpg"; // unifuse matterport, depth-anyting metterport
						}
						//fn_equirectangular_baseline = baseline_folder + rawname + "_depth_pred.png"; // Zoe matterport, depth-anyting metric raw matterport
						if (baseline == "hohonet") {
							fn_equirectangular_baseline = baseline_folder + rawname + "_rgb.depth.png"; // hohonet matterport
						}
						if (baseline == "egformer" || baseline == "panoformer") {
							fn_equirectangular_baseline = baseline_folder + rawname + ".png"; // egformer panoformer matterport
						}
						//fn_equirectangular_baseline = baseline_folder + rawname + ".depth.png"; // hohonet replica
						if (false) {
							if (result_folder.find("Slicenet") != string::npos || result_folder.find("slicenet") != string::npos)
							{
								fn_equirectangular_baseline = baseline_folder + rawname + ".jpg.slicenet.png";  //matterport or replica360
								//fn_equirectangular_baseline = baseline_folder + rawname + ".png.slicenet.png";  //stanford2d3d or suncg
							}
							else if (result_folder.find("unifuse") != string::npos)
							{
								fn_equirectangular_baseline = baseline_folder + rawname + ".unifuse.jpg";
							}
							else if (result_folder.find("hohonet") != string::npos)
							{
								//hohonet:
								fn_equirectangular_baseline = baseline_folder + rawname + ".depth.png";
							}
						}

						////gt filename:
						{
							//default matterport way:
							//fn_equirectangular_gt = gt_folder + rawname + ".png"; 
							fn_equirectangular_gt = gt_folder + rawname + "_depth_gt.jpg"; // matterport

							//stanford2d3d: replace "_rgb" with "_depth: 
							{
								size_t index = fn_equirectangular_gt.find("_rgb");
								if (index != string::npos)
									fn_equirectangular_gt.replace(index, 4, "_depth");
							}

							//suncg:				
							if (false)
							{
								fn_equirectangular_gt = gt_folder + rawname + ".exr.png";
								size_t index = fn_equirectangular_gt.find("_color");
								if (index != string::npos)
									fn_equirectangular_gt.replace(index, 6, "_depth");
							}

							//replica:
							if (replica)
							{
								fn_equirectangular_gt = gt_folder + rawname + ".pfm";
								//replace "rgb" by "depth"
								size_t pos = fn_equirectangular_gt.find("rgb");
								if (pos != std::string::npos)
								{
									fn_equirectangular_gt.replace(pos, 3, "depth");
								}
							}
						}
					}

					string output_filename = result_folder + rawname + ".png";

					//note: if the output file already exists, skip?
					{
						FILE* fp = fopen(output_filename.c_str(), "r");
						if (fp != NULL)
						{
							cout << i << "/" << RGB_filenames.size() << " skip!" << endl;
							fclose(fp);
							continue;
						}
					}

					cout << i << "/" << RGB_filenames.size() << " baseline:" << fn_equirectangular_baseline << endl;
					cout << "gt:" << fn_equirectangular_gt << endl;
					cout << "output_filename:" << output_filename << endl;

					//string test_img_folder = "test_images_matterport3d/test_images_depthAnyting_metric_raw_margin10/";

					cout << "test_image_folder: " << test_img_folder << endl;

					vector<string> pmap_fns;  //filename of the perspective RGB images
					cout << "g_cubemap_FOVs.size()" << g_cubemap_FOVs.size() << endl;

					for (int i = 0; i < g_cubemap_FOVs.size(); i++)
					{
						//prepare filename:
						char filename[260] = { NULL };

						//MiDas pmaps:
						/*sprintf(filename, "%s.%d_%d_%d_%d.png", (string("output/") + rawname).c_str(),
							(int)round(R2D(g_cubemap_FOVs[i][0])), (int)round(R2D(g_cubemap_FOVs[i][1])),
							(int)round(R2D(g_cubemap_FOVs[i][2])), (int)round(R2D(g_cubemap_FOVs[i][3])));*/

							//LeRes pmaps:
						if (leres || zoedepth || depthAnything) {
							/*sprintf(filename, "%s.%d_%d_%d_%d.png", (string("test_images/") + rawname).c_str(),
								(int)round(R2D(g_cubemap_FOVs[i][0])), (int)round(R2D(g_cubemap_FOVs[i][1])),
								(int)round(R2D(g_cubemap_FOVs[i][2])), (int)round(R2D(g_cubemap_FOVs[i][3])));*/

								// matterport3d
							sprintf(filename, "%s.%d_%d_%d_%d.png", (test_img_folder + rawname).c_str(),
								(int)round(R2D(g_cubemap_FOVs[i][0])), (int)round(R2D(g_cubemap_FOVs[i][1])),
								(int)round(R2D(g_cubemap_FOVs[i][2])), (int)round(R2D(g_cubemap_FOVs[i][3])));

							// replica leres
							/*string target = "_pano";
							string rawname_test_images = rawname;
							size_t pos = rawname_test_images.find(target);
							rawname_test_images.erase(pos, target.length());

							sprintf(filename, "%s.%d_%d_%d_%d.png", (string("test_images_replica360_2k/test_images_leres/") + rawname_test_images).c_str(),
								(int)round(R2D(g_cubemap_FOVs[i][0])), (int)round(R2D(g_cubemap_FOVs[i][1])),
								(int)round(R2D(g_cubemap_FOVs[i][2])), (int)round(R2D(g_cubemap_FOVs[i][3])));*/

							pmap_fns.push_back(filename);
							//cout << filename << endl;
						}
						/*else if (zoedepth) {
							sprintf(filename, "%s.%d_%d_%d_%d.png", (string("test_images/") + rawname).c_str(),
								(int)round(R2D(g_cubemap_FOVs[i][0])), (int)round(R2D(g_cubemap_FOVs[i][1])),
								(int)round(R2D(g_cubemap_FOVs[i][2])), (int)round(R2D(g_cubemap_FOVs[i][3])));

							pmap_fns.push_back(filename);
						}*/
						else {
							sprintf(filename, "%s.%d_%d_%d_%d.jpg", (string("test_images/") + rawname).c_str(),
								(int)round(R2D(g_cubemap_FOVs[i][0])), (int)round(R2D(g_cubemap_FOVs[i][1])),
								(int)round(R2D(g_cubemap_FOVs[i][2])), (int)round(R2D(g_cubemap_FOVs[i][3])));

							pmap_fns.push_back(filename);
						}//cout << "filename" << filename << endl;
					}

					//generate the merged depth panorama!
					DepthNamespace::Metrics metrics_aligned;
					int time_Reg = 0, time_Laplacian = 0;
					if (!DepthNamespace::MergeDepthMaps(fn_equirectangular_baseline, pmap_fns, output_filename, result_folder, rawname, g_cubemap_FOVs, g_cubemap_ranges,
						2048, g_zenith_range, &fn_equirectangular_gt, &metrics_aligned, &time_Reg, &time_Laplacian))
					{
						cout << "[CreateDepthPanorma] MergeDepthMaps #" << i << " failed!" << endl;
						return false;
					}


					time_Regs.push_back(time_Reg);
					time_Laplacians.push_back(time_Laplacian);

					char metrics_aligned_filename[260] = { NULL };
					sprintf(metrics_aligned_filename, "%s.aligned.txt", (result_folder + rawname).c_str());
					metrics_aligned.Save(metrics_aligned_filename);
					metrics_aligned_all.push_back(metrics_aligned);

					//report avg metrics so far
					if (i == RGB_filenames.size() - 1 || (i > 0 && i % 5 == 0))
					{
						cout << "----------" << endl;
						for (int Case = 0; Case < 1; Case++)
						{
							vector<DepthNamespace::Metrics>* metrics_all = NULL;
							metrics_all = &metrics_aligned_all;

							float rmse_given_avg = 0, rmse_result_avg = 0, mae_given_avg = 0, mae_result_avg = 0,
								mre_given_avg = 0, mre_result_avg = 0, rmselog_given_avg = 0, rmselog_result_avg = 0;
							float delta1_given_avg = 0, delta1_result_avg = 0, delta2_given_avg = 0, delta2_result_avg = 0, delta3_given_avg = 0, delta3_result_avg = 0;
							for (int i = 0; i < (*metrics_all).size(); i++)
							{
								rmse_given_avg += sqrt((*metrics_all)[i].mse_given);
								rmse_result_avg += sqrt((*metrics_all)[i].mse_result);
								mae_given_avg += (*metrics_all)[i].mae_given;
								mae_result_avg += (*metrics_all)[i].mae_result;
								mre_given_avg += (*metrics_all)[i].mre_given;
								mre_result_avg += (*metrics_all)[i].mre_result;
								rmselog_given_avg += sqrt((*metrics_all)[i].mselog_given);
								rmselog_result_avg += sqrt((*metrics_all)[i].mselog_result);
								delta1_given_avg += (*metrics_all)[i].delta1_given;
								delta1_result_avg += (*metrics_all)[i].delta1_result;
								delta2_given_avg += (*metrics_all)[i].delta2_given;
								delta2_result_avg += (*metrics_all)[i].delta2_result;
								delta3_given_avg += (*metrics_all)[i].delta3_given;
								delta3_result_avg += (*metrics_all)[i].delta3_result;
							}
							rmse_given_avg /= (float)(*metrics_all).size();
							rmse_result_avg /= (float)(*metrics_all).size();
							mae_given_avg /= (float)(*metrics_all).size();
							mae_result_avg /= (float)(*metrics_all).size();
							mre_given_avg /= (float)(*metrics_all).size();
							mre_result_avg /= (float)(*metrics_all).size();
							rmselog_given_avg /= (float)(*metrics_all).size();
							rmselog_result_avg /= (float)(*metrics_all).size();
							delta1_given_avg /= (float)(*metrics_all).size();
							delta1_result_avg /= (float)(*metrics_all).size();
							delta2_given_avg /= (float)(*metrics_all).size();
							delta2_result_avg /= (float)(*metrics_all).size();
							delta3_given_avg /= (float)(*metrics_all).size();
							delta3_result_avg /= (float)(*metrics_all).size();

							cout << "RMSE_given:" << rmse_given_avg << " RMSE_result:" << rmse_result_avg << " (" <<
								(rmse_result_avg - rmse_given_avg) / rmse_given_avg << ")" <<
								" MAE_given:" << mae_given_avg << " MAE_result_avg:" << mae_result_avg << " (" <<
								(mae_result_avg - mae_given_avg) / mae_given_avg << ")" <<
								" MRE_given:" << mre_given_avg << " MRE_result_avg:" << mre_result_avg << " (" <<
								(mre_result_avg - mre_given_avg) / mre_given_avg << ")" <<
								" RMSElog_given:" << rmselog_given_avg << " RMSElog_result:" << rmselog_result_avg << " (" <<
								(rmselog_result_avg - rmselog_given_avg) / rmselog_given_avg << ")" <<
								" delta1_given:" << delta1_given_avg << " delta1_result:" << delta1_result_avg << " (" <<
								(delta1_result_avg - delta1_given_avg) << ")" <<
								" delta2_given:" << delta2_given_avg << " delta2_result:" << delta2_result_avg << " (" <<
								(delta2_result_avg - delta2_given_avg) << ")" <<
								" delta3_given:" << delta3_given_avg << " delta3_result:" << delta3_result_avg << " (" <<
								(delta3_result_avg - delta3_given_avg) << ")" << endl;

							cout << "(*metrics_all).size() " << (*metrics_all).size() << endl;

							if (save_metric && i == RGB_filenames.size() - 1 && (*metrics_all).size() == RGB_filenames.size()) {
								ofstream outFile(output_txt);

								if (!outFile) {
									std::cerr << "fail to open file" << std::endl;
									return 1;
								}
								outFile << " RMSE: " << rmse_result_avg << endl;
								outFile << " MAE: " << mae_result_avg << endl;
								outFile << " MRE: " << mre_result_avg << endl;
								outFile << " RMSElog: " << rmselog_result_avg << endl;
								outFile << " delta1: " << delta1_result_avg << endl;
								outFile << " delta2: " << delta2_result_avg << endl;
								outFile << " delta3: " << delta3_result_avg << endl;

								outFile.close();

								cout << "save metric at " << output_txt << endl;
							}
						}

						float time_Reg_avg = 0;
						for (int ii = 0; ii < time_Regs.size(); ii++)
						{
							time_Reg_avg += time_Regs[ii];
						}
						time_Reg_avg /= (float)time_Regs.size();

						float time_Laplacian_avg = 0;
						for (int ii = 0; ii < time_Laplacians.size(); ii++)
						{
							time_Laplacian_avg += time_Laplacians[ii];
						}
						time_Laplacian_avg /= (float)time_Laplacians.size();

						cout << "time_Reg_avg:" << time_Reg_avg << " time_Laplacian_avg:" << time_Laplacian_avg << endl;

						cout << "----------" << endl;
					}
				}
				FILE* fp = fopen(output_txt.c_str(), "r");
				vector<string> RGB_filenames;
				AllFilesInFolder(RGB_folder.c_str(), RGB_filenames);

				if (fp == NULL) {
					cout << output_txt << " doesnt exist" << endl;
					cout << result_folder << " cal metrics" << endl;
					bool replica = false;

					// matterport3d
					//string gt_folder = ".\\datas\\matterport3d\\gt_grayscale\\";
					string pred_folder = result_folder;
					//string pred_folder = ".\\datas\\matterport3d\\depthAnything_V2_metric\\result\\"; // baseline 
					//output_txt = ".\\record\\matterport3d\\baseline_unifuse\\zoe_raw\\metrics.txt";
					//string output_txt = ".\\datas\\matterport3d\\depthAnything_V2_metric\\result\\metrics.txt";

					// replica360 2k
					//replica = true;
					//string gt_folder = ".\\datas\\replica360_2k\\replica3602k_gt\\";
					//string pred_folder = ".\\record\\replica360_2k\\baseline_hohonet\\leres\\\\result\\"; // hohonet
					//string output_txt = ".\\record\\replica360_2k\\baseline_hohonet\\leres\\\\result\\\\metrics.txt"; //hohonet

					vector<string> gts;
					AllFilesInFolder(gt_folder.c_str(), gts);
					//cout << gts[0] << endl;


					float rmse_given_avg = 0, rmse_result_avg = 0, mae_given_avg = 0, mae_result_avg = 0,
						mre_given_avg = 0, mre_result_avg = 0, rmselog_given_avg = 0, rmselog_result_avg = 0;
					float delta1_given_avg = 0, delta1_result_avg = 0, delta2_given_avg = 0, delta2_result_avg = 0, delta3_given_avg = 0, delta3_result_avg = 0;

					float img_num = 0, load_error_num = 0;

					for (auto gt : gts) {
						string pred;
						string rawname;
						char* filename = PathFindFileNameA(gt.c_str());

						// matterport3d
						rawname = string(filename).substr(0, 4);
						pred = pred_folder + rawname + ".png"; //for merged pred pano
						//pred = pred_folder + rawname + "_rgb.depth.png"; // hohonet
						//pred = pred_folder + rawname + "_depth_pred.jpg"; //for pred of baseline of unifuse / depthAnything
						//pred = pred_folder + rawname + "_depth_pred.png"; //for pred of baseline of zoe / depthAnything metric

						// replica360 2k
						//std::string suffix = "_depth_pano.pfm";
						//size_t suffixPos = string(filename).find(suffix);
						//rawname = string(filename).substr(0, suffixPos);
						//pred = pred_folder + rawname + "_rgb_pano.png"; // leres (hohonet)
						//pred = pred_folder + rawname + "_rgb_pano.depth.png"; // hohonet


						int align_way = 1;
						//align_way = 0; // no align
						//load the ground-truth emap:
						EquirectangularMap emap_gt;
						EquirectangularMap emap_pred;

						vector<DepthNamespace::Metrics> metrics_aligned_all;
						DepthNamespace::Metrics metrics_aligned;

						bool cap_depth = true, DispDepthCompare = false;

						//the pred is disp
						if (DispDepthCompare) {
							string shifted_filename_str = pred_folder + "shifted\\\\" + rawname + "_depth_pred.png";
							const char* shifted_filename = shifted_filename_str.c_str();
							ErrorCompare(gt, pred, DispDepthCompare, metrics_aligned.mse_result, metrics_aligned.mae_result, metrics_aligned.mre_result,
								metrics_aligned.mselog_result, metrics_aligned.delta1_result, metrics_aligned.delta2_result, metrics_aligned.delta3_result, align_way, cap_depth, shifted_filename);

							metrics_aligned_all.push_back(metrics_aligned);


							rmse_given_avg += sqrt(metrics_aligned.mse_given);
							rmse_result_avg += sqrt(metrics_aligned.mse_result);
							mae_given_avg += metrics_aligned.mae_given;
							mae_result_avg += metrics_aligned.mae_result;
							mre_given_avg += metrics_aligned.mre_given;
							mre_result_avg += metrics_aligned.mre_result;
							rmselog_given_avg += sqrt(metrics_aligned.mselog_given);
							rmselog_result_avg += sqrt(metrics_aligned.mselog_result);
							delta1_given_avg += metrics_aligned.delta1_given;
							delta1_result_avg += metrics_aligned.delta1_result;
							delta2_given_avg += metrics_aligned.delta2_given;
							delta2_result_avg += metrics_aligned.delta2_result;
							delta3_given_avg += metrics_aligned.delta3_given;
							delta3_result_avg += metrics_aligned.delta3_result;

							img_num++;
						}
						else {
							if (emap_gt.Load((gt)) && emap_pred.Load((pred)))
							{
								//emap_pred.DispDepthConversion(); // zoe, convert disp to depth
								ErrorEmap(emap_gt, emap_pred, metrics_aligned.mse_result, metrics_aligned.mae_result, metrics_aligned.mre_result,
									metrics_aligned.mselog_result, metrics_aligned.delta1_result, metrics_aligned.delta2_result, metrics_aligned.delta3_result, align_way, cap_depth);

								metrics_aligned_all.push_back(metrics_aligned);


								rmse_given_avg += sqrt(metrics_aligned.mse_given);
								rmse_result_avg += sqrt(metrics_aligned.mse_result);
								mae_given_avg += metrics_aligned.mae_given;
								mae_result_avg += metrics_aligned.mae_result;
								mre_given_avg += metrics_aligned.mre_given;
								mre_result_avg += metrics_aligned.mre_result;
								rmselog_given_avg += sqrt(metrics_aligned.mselog_given);
								rmselog_result_avg += sqrt(metrics_aligned.mselog_result);
								delta1_given_avg += metrics_aligned.delta1_given;
								delta1_result_avg += metrics_aligned.delta1_result;
								delta2_given_avg += metrics_aligned.delta2_given;
								delta2_result_avg += metrics_aligned.delta2_result;
								delta3_given_avg += metrics_aligned.delta3_given;
								delta3_result_avg += metrics_aligned.delta3_result;

								img_num++;

							}
							else {
								cout << "load error" << endl;
								load_error_num++; // replica360 2k / there are some imgs cannt load
								if (!replica) {
									return true;
								}
							}
						}
						cout << gt << "   " << pred << endl;
					}

					rmse_given_avg /= img_num;
					rmse_result_avg /= img_num;
					mae_given_avg /= img_num;
					mae_result_avg /= img_num;
					mre_given_avg /= img_num;
					mre_result_avg /= img_num;
					rmselog_given_avg /= img_num;
					rmselog_result_avg /= img_num;
					delta1_given_avg /= img_num;
					delta1_result_avg /= img_num;
					delta2_given_avg /= img_num;
					delta2_result_avg /= img_num;
					delta3_given_avg /= img_num;
					delta3_result_avg /= img_num;

					cout << "RMSE_given:" << rmse_given_avg << " RMSE_result:" << rmse_result_avg << " (" <<
						(rmse_result_avg - rmse_given_avg) / rmse_given_avg << ")" <<
						" MAE_given:" << mae_given_avg << " MAE_result_avg:" << mae_result_avg << " (" <<
						(mae_result_avg - mae_given_avg) / mae_given_avg << ")" <<
						" MRE_given:" << mre_given_avg << " MRE_result_avg:" << mre_result_avg << " (" <<
						(mre_result_avg - mre_given_avg) / mre_given_avg << ")" <<
						" RMSElog_given:" << rmselog_given_avg << " RMSElog_result:" << rmselog_result_avg << " (" <<
						(rmselog_result_avg - rmselog_given_avg) / rmselog_given_avg << ")" <<
						" delta1_given:" << delta1_given_avg << " delta1_result:" << delta1_result_avg << " (" <<
						(delta1_result_avg - delta1_given_avg) << ")" <<
						" delta2_given:" << delta2_given_avg << " delta2_result:" << delta2_result_avg << " (" <<
						(delta2_result_avg - delta2_given_avg) << ")" <<
						" delta3_given:" << delta3_given_avg << " delta3_result:" << delta3_result_avg << " (" <<
						(delta3_result_avg - delta3_given_avg) << ")" << endl;

					ofstream outFile(output_txt);

					if (!outFile) {
						std::cerr << "fail to open file" << std::endl;
						return 1;
					}
					outFile << " RMSE: " << rmse_result_avg << endl;
					outFile << " MAE: " << mae_result_avg << endl;
					outFile << " MRE: " << mre_result_avg << endl;
					outFile << " RMSElog: " << rmselog_result_avg << endl;
					outFile << " delta1: " << delta1_result_avg << endl;
					outFile << " delta2: " << delta2_result_avg << endl;
					outFile << " delta3: " << delta3_result_avg << endl;

					outFile.close();

					cout << "error counts: " << load_error_num << endl;

					//return true;
				}
			}
		}
	}
	if (!skip_createPano) {

		glfwTerminate();
	}
	return true;
}


int main(int argc, char* argv[])
{
	bool shift_all = false, shift_v = false, shift_h = false, no_shift = false, margin10 = false, fold8 = false, fold6 = false, fold4 = true;
	bool margin_quarter = false;
	bool metric_cal_only = false, skip_createPano = true, skip_depth_pred = true, skip_merge = false;
	bool output2file = false;
	ofstream outfile;
	streambuf* coutbuf = nullptr;
	if (output2file) {
		outfile.open(".\\record\\leres_6fold\\output.txt");
		coutbuf = cout.rdbuf();
		cout.rdbuf(outfile.rdbuf());
	}
	//setup cubemap setting
	if (false)
	{
		//4-fold 

		//up (zenith center is 63)
		g_cubemap_FOVs.push_back(Vec4f(D2R(-2), D2R(92), D2R(17), D2R(109)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(88), D2R(182), D2R(17), D2R(109)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(178), D2R(272), D2R(17), D2R(109)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(268), D2R(362), D2R(17), D2R(109)));
		//middle
		g_cubemap_FOVs.push_back(Vec4f(D2R(-2), D2R(92), D2R(44), D2R(136)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(88), D2R(182), D2R(44), D2R(136)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(178), D2R(272), D2R(44), D2R(136)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(268), D2R(362), D2R(44), D2R(136)));
		//down (zenith center is 117)
		g_cubemap_FOVs.push_back(Vec4f(D2R(-2), D2R(92), D2R(71), D2R(163)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(88), D2R(182), D2R(71), D2R(163)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(178), D2R(272), D2R(71), D2R(163)));
		g_cubemap_FOVs.push_back(Vec4f(D2R(268), D2R(362), D2R(71), D2R(163)));

		//"up"
		g_cubemap_ranges.push_back(Vec4f(D2R(90), D2R(0), D2R(25), D2R(56)));
		g_cubemap_ranges.push_back(Vec4f(D2R(180), D2R(90), D2R(25), D2R(56)));
		g_cubemap_ranges.push_back(Vec4f(D2R(270), D2R(180), D2R(25), D2R(56)));
		g_cubemap_ranges.push_back(Vec4f(D2R(360), D2R(270), D2R(25), D2R(56)));
		//"middle"
		g_cubemap_ranges.push_back(Vec4f(D2R(90), D2R(0), D2R(56), D2R(124)));
		g_cubemap_ranges.push_back(Vec4f(D2R(180), D2R(90), D2R(56), D2R(124)));
		g_cubemap_ranges.push_back(Vec4f(D2R(270), D2R(180), D2R(56), D2R(124)));
		g_cubemap_ranges.push_back(Vec4f(D2R(360), D2R(270), D2R(56), D2R(124)));
		//"down"
		g_cubemap_ranges.push_back(Vec4f(D2R(90), D2R(0), D2R(124), D2R(155)));
		g_cubemap_ranges.push_back(Vec4f(D2R(180), D2R(90), D2R(124), D2R(155)));
		g_cubemap_ranges.push_back(Vec4f(D2R(270), D2R(180), D2R(124), D2R(155)));
		g_cubemap_ranges.push_back(Vec4f(D2R(360), D2R(270), D2R(124), D2R(155)));
	}
	if (false)
	{
		//5-fold for MiDas

		float margin = D2R(2);
		float azi00 = D2R(0) - margin, azi01 = D2R(72) + margin;
		float azi10 = D2R(72) - margin, azi11 = D2R(144) + margin;
		float azi20 = D2R(144) - margin, azi21 = D2R(216) + margin;
		float azi30 = D2R(216) - margin, azi31 = D2R(288) + margin;
		float azi40 = D2R(288) - margin, azi41 = D2R(360) + margin;

		//up (zenith center is 49)
		float zen00 = D2R(20), zen01 = D2R(78);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		//middle (zenith center is 90)
		float zen10 = D2R(61), zen11 = D2R(119);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		//down (zenith center is 131)
		float zen20 = D2R(102), zen21 = D2R(160);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));

		////ranges:

		//"up"
		float Zen00 = D2R(25), Zen01 = D2R(67);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
		//"middle"
		float Zen10 = D2R(67), Zen11 = D2R(113);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
		//"down"
		float Zen20 = D2R(113), Zen21 = D2R(155);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
	}
	if (fold4) {
		//4-fold
		float margin = D2R(3);
		if (margin_quarter) margin = D2R(15); //2x / (2x + 90) = 1 / 4 ==> x = 15

		float azi00, azi01;
		float azi10, azi11;
		float azi20, azi21;
		float azi30, azi31;
		float azi40, azi41;
		float azi50, azi51;

		{

			azi00 = D2R(0) - margin, azi01 = D2R(90) + margin;
			azi10 = D2R(90) - margin, azi11 = D2R(180) + margin;
			azi20 = D2R(180) - margin, azi21 = D2R(270) + margin;
			azi30 = D2R(270) - margin, azi31 = D2R(360) + margin;
			/*azi40 = D2R(240) - margin, azi41 = D2R(300) + margin;
			azi50 = D2R(300) - margin, azi51 = D2R(360) + margin;*/
		}

		//up (zenith center is 56)
		//float zen00 = D2R(18), zen01 = D2R(94);
		//if use the original zenith, the pmap will out of range of zenith when do the sphericalto2d
		float zen00 = D2R(17), zen01 = D2R(109);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		/*g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen00, zen01));*/
		//middle (zenith center is 90)
		//float zen10 = D2R(52), zen11 = D2R(128);
		float zen10 = D2R(44), zen11 = D2R(136);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		/*g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen10, zen11));*/
		//down (zenith center is 124)
		//float zen20 = D2R(86), zen21 = D2R(162);
		float zen20 = D2R(77), zen21 = D2R(163);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		/*g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen20, zen21));*/

		////ranges;
		{

			//"up"
			//float Zen00 = D2R(25), Zen01 = D2R(60);
			float Zen00 = D2R(25), Zen01 = D2R(56);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
			/*g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen00, Zen01));*/
			//"middle"
			//float Zen10 = D2R(60), Zen11 = D2R(120);
			float Zen10 = D2R(56), Zen11 = D2R(124);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
			/*g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen10, Zen11));*/
			//"down"
			//float Zen20 = D2R(120), Zen21 = D2R(155);
			float Zen20 = D2R(124), Zen21 = D2R(155);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
			/*g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen20, Zen21));*/
		}
	}
	if (fold6) {
		//6-fold
		float margin = D2R(3);
		if (margin10) margin = D2R(10);

		float azi00, azi01;
		float azi10, azi11;
		float azi20, azi21;
		float azi30, azi31;
		float azi40, azi41;
		float azi50, azi51;

		{

			azi00 = D2R(0) - margin, azi01 = D2R(60) + margin;
			azi10 = D2R(60) - margin, azi11 = D2R(120) + margin;
			azi20 = D2R(120) - margin, azi21 = D2R(180) + margin;
			azi30 = D2R(180) - margin, azi31 = D2R(240) + margin;
			azi40 = D2R(240) - margin, azi41 = D2R(300) + margin;
			azi50 = D2R(300) - margin, azi51 = D2R(360) + margin;
		}

		//up (zenith center is 56)
		float zen00 = D2R(18), zen01 = D2R(94);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen00, zen01));
		//middle (zenith center is 90)
		float zen10 = D2R(52), zen11 = D2R(128);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen10, zen11));
		//down (zenith center is 124)
		float zen20 = D2R(86), zen21 = D2R(162);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen20, zen21));

		////ranges;
		{

			//"up"
			float Zen00 = D2R(25), Zen01 = D2R(60);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen00, Zen01));
			//"middle"
			float Zen10 = D2R(60), Zen11 = D2R(120);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen10, Zen11));
			//"down"
			float Zen20 = D2R(120), Zen21 = D2R(155);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen20, Zen21));
		}
	}
	if (fold8) {
		//8-fold
		float margin = D2R(3);
		if (margin10) margin = D2R(10);
		float azi00 = D2R(0) - margin, azi01 = D2R(45) + margin;
		float azi10 = D2R(45) - margin, azi11 = D2R(90) + margin;
		float azi20 = D2R(90) - margin, azi21 = D2R(135) + margin;
		float azi30 = D2R(135) - margin, azi31 = D2R(180) + margin;
		float azi40 = D2R(180) - margin, azi41 = D2R(225) + margin;
		float azi50 = D2R(225) - margin, azi51 = D2R(270) + margin;
		float azi60 = D2R(270) - margin, azi61 = D2R(315) + margin;
		float azi70 = D2R(315) - margin, azi71 = D2R(360) + margin;

		//up (zenith center is 56)
		float zen00 = D2R(18), zen01 = D2R(94);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi60, azi61, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi70, azi71, zen00, zen01));
		//middle (zenith center is 90)
		float zen10 = D2R(52), zen11 = D2R(128);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi60, azi61, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi70, azi71, zen10, zen11));
		//down (zenith center is 124)
		float zen20 = D2R(86), zen21 = D2R(162);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi60, azi61, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi70, azi71, zen20, zen21));

		////ranges:

		//"up"
		float Zen00 = D2R(25), Zen01 = D2R(60);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi61 - margin, azi60 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi71 - margin, azi70 + margin, Zen00, Zen01));
		//"middle"
		float Zen10 = D2R(60), Zen11 = D2R(120);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi61 - margin, azi60 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi71 - margin, azi70 + margin, Zen10, Zen11));
		//"down"
		float Zen20 = D2R(120), Zen21 = D2R(155);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi61 - margin, azi60 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi71 - margin, azi70 + margin, Zen20, Zen21));
	}

	if (shift_h) {
		//6-fold
		float margin = D2R(3);
		float azi00, azi01;
		float azi10, azi11;
		float azi20, azi21;
		float azi30, azi31;
		float azi40, azi41;
		float azi50, azi51;

		{
			// shift horizental
			float shift_a = D2R(36);

			azi00 = D2R(0) - margin, azi01 = D2R(72) + margin - shift_a;
			azi10 = D2R(72) - margin - shift_a, azi11 = D2R(144) + margin - shift_a;
			azi20 = D2R(144) - margin - shift_a, azi21 = D2R(216) + margin - shift_a;
			azi30 = D2R(216) - margin - shift_a, azi31 = D2R(288) + margin - shift_a;
			azi40 = D2R(288) - margin - shift_a, azi41 = D2R(360) + margin - shift_a;
			azi50 = D2R(360) - margin - shift_a, azi51 = D2R(360) + margin;
		}

		//up (zenith center is 56)
		float zen00 = D2R(18), zen01 = D2R(94);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen00, zen01));
		//middle (zenith center is 90)
		float zen10 = D2R(52), zen11 = D2R(128);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen10, zen11));
		//down (zenith center is 124)
		float zen20 = D2R(86), zen21 = D2R(162);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen20, zen21));

		////ranges;
		{

			//"up"
			float Zen00 = D2R(25), Zen01 = D2R(60);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen00, Zen01));
			//"middle"
			float Zen10 = D2R(60), Zen11 = D2R(120);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen10, Zen11));
			//"down"
			float Zen20 = D2R(120), Zen21 = D2R(155);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen20, Zen21));
		}
	}

	if (shift_v)
	{
		//5-fold for LeRes

		float margin = D2R(3);

		float azi00 = D2R(0) - margin, azi01 = D2R(72) + margin;
		float azi10 = D2R(72) - margin, azi11 = D2R(144) + margin;
		float azi20 = D2R(144) - margin, azi21 = D2R(216) + margin;
		float azi30 = D2R(216) - margin, azi31 = D2R(288) + margin;
		float azi40 = D2R(288) - margin, azi41 = D2R(360) + margin;

		//up (zenith center is 56)
		float zen00 = D2R(18), zen01 = D2R(94);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		//middle (zenith center is 90)
		float zen10 = D2R(52), zen11 = D2R(128);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		//down (zenith center is 124)
		float zen20 = D2R(86), zen21 = D2R(162);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));

		////ranges:
		// 
		// shift vertical
		float shift_z = D2R(10);
		//"up"
		float Zen00 = D2R(25), Zen01 = D2R(60) + shift_z;
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
		//"middle"
		float Zen10 = D2R(60) + shift_z, Zen11 = D2R(120) - shift_z;
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
		//"down"
		float Zen20 = D2R(120) - shift_z, Zen21 = D2R(155);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
	}

	if (shift_all) {
		//6-fold
		float margin = D2R(3);
		float azi00, azi01;
		float azi10, azi11;
		float azi20, azi21;
		float azi30, azi31;
		float azi40, azi41;
		float azi50, azi51;

		{
			// shift horizental
			float shift_a = D2R(36);

			azi00 = D2R(0) - margin, azi01 = D2R(72) + margin - shift_a;
			azi10 = D2R(72) - margin - shift_a, azi11 = D2R(144) + margin - shift_a;
			azi20 = D2R(144) - margin - shift_a, azi21 = D2R(216) + margin - shift_a;
			azi30 = D2R(216) - margin - shift_a, azi31 = D2R(288) + margin - shift_a;
			azi40 = D2R(288) - margin - shift_a, azi41 = D2R(360) + margin - shift_a;
			azi50 = D2R(360) - margin - shift_a, azi51 = D2R(360) + margin;
		}

		//up (zenith center is 56)
		float zen00 = D2R(18), zen01 = D2R(94);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen00, zen01));
		//middle (zenith center is 90)
		float zen10 = D2R(52), zen11 = D2R(128);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen10, zen11));
		//down (zenith center is 124)
		float zen20 = D2R(86), zen21 = D2R(162);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi50, azi51, zen20, zen21));


		////ranges;
		{
			// shift vertical
			float shift_z = D2R(10);

			//"up"
			float Zen00 = D2R(25), Zen01 = D2R(60) + shift_z;
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen00, Zen01));
			//"middle"
			float Zen10 = D2R(60) + shift_z, Zen11 = D2R(120) - shift_z;
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen10, Zen11));
			//"down"
			float Zen20 = D2R(120) - shift_z, Zen21 = D2R(155);
			g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
			g_cubemap_ranges.push_back(Vec4f(azi51 - margin, azi50 + margin, Zen20, Zen21));
		}
	}
	if (no_shift)
	{
		//5-fold for LeRes
		float margin = D2R(3);
		if (margin10) margin = D2R(10);

		float azi00 = D2R(0) - margin, azi01 = D2R(72) + margin;
		float azi10 = D2R(72) - margin, azi11 = D2R(144) + margin;
		float azi20 = D2R(144) - margin, azi21 = D2R(216) + margin;
		float azi30 = D2R(216) - margin, azi31 = D2R(288) + margin;
		float azi40 = D2R(288) - margin, azi41 = D2R(360) + margin;

		//up (zenith center is 56)
		float zen00 = D2R(18), zen01 = D2R(94);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen00, zen01));
		//middle (zenith center is 90)
		float zen10 = D2R(52), zen11 = D2R(128);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen10, zen11));
		//down (zenith center is 124)
		float zen20 = D2R(86), zen21 = D2R(162);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi30, azi31, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi40, azi41, zen20, zen21));

		////ranges:

		//"up"
		float Zen00 = D2R(25), Zen01 = D2R(60);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen00, Zen01));
		//"middle"
		float Zen10 = D2R(60), Zen11 = D2R(120);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen10, Zen11));
		//"down"
		float Zen20 = D2R(120), Zen21 = D2R(155);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi31 - margin, azi30 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi41 - margin, azi40 + margin, Zen20, Zen21));
	}
	if (false)
	{
		//3-fold 

		float margin = D2R(2);
		float azi00 = D2R(0) - margin, azi01 = D2R(120) + margin;
		float azi10 = D2R(120) - margin, azi11 = D2R(240) + margin;
		float azi20 = D2R(240) - margin, azi21 = D2R(360) + margin;

		//up (zen center at 66)
		float zen00 = D2R(12), zen01 = D2R(120);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen00, zen01));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen00, zen01));
		//middle
		float zen10 = D2R(36), zen11 = D2R(144);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen10, zen11));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen10, zen11));
		//down (zen center at 114)
		float zen20 = D2R(60), zen21 = D2R(168);
		g_cubemap_FOVs.push_back(Vec4f(azi00, azi01, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi10, azi11, zen20, zen21));
		g_cubemap_FOVs.push_back(Vec4f(azi20, azi21, zen20, zen21));

		////ranges:

		//"up"
		float Zen00 = D2R(26), Zen01 = D2R(60);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen00, Zen01));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen00, Zen01));
		//"middle"
		float Zen10 = D2R(60), Zen11 = D2R(120);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen10, Zen11));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen10, Zen11));
		//"down"
		float Zen20 = D2R(120), Zen21 = D2R(154);
		g_cubemap_ranges.push_back(Vec4f(azi01 - margin, azi00 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi11 - margin, azi10 + margin, Zen20, Zen21));
		g_cubemap_ranges.push_back(Vec4f(azi21 - margin, azi20 + margin, Zen20, Zen21));
	}

	//command line mode:
	if (argc > 1)
	{
		string cmd(argv[1]);

		if (cmd == "0")
			CreateDepthPanoramas(argc, argv, metric_cal_only, skip_createPano, skip_depth_pred, skip_merge);
		//else if (cmd == "1")
		//	AnalaysisResult(argc, argv, false/*mono360?*/);		

		return 0;
	}
	if (output2file) {
		cout.rdbuf(coutbuf);
		outfile.close();
	}
	return 0;
}